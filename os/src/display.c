/*
 * display.c — 显示编排：LVGL 驱动 + 帧合成 + 策略串联（C99）
 *
 * 分层（依赖单向向下，移植只需改最底层）：
 *
 *   LVGL 驱动   本文件上半：display device + flush_cb
 *        |              LVGL 交出 L8 区域 -> 交给 raster 阈值化
 *        v
 *   raster_1bpp 纯函数：L8 rect -> 1bpp 条带比特；脏区计算
 *        |
 *        v
 *   display_policy  纯决策：这一帧刷不刷、用什么波形
 *        |
 *        v
 *   panel        唯一碰设备的一层：整帧 write + refresh sysfs
 *
 * 局部刷新说明：MP-D261 驱动只接受整帧 write(5624)，没有区域参数。
 * 所以「局部刷新」在这里的含义是 **提交决策**——用脏区判断是否值得动屏
 * （无变化就完全不刷，省 e-ink 寿命与功耗），以及按脏区大小选波形。
 * 字节级的区域写入需要换支持区域参数的控制器，那时 panel 层加一个
 * variant 即可，上层不用动。
 *
 * 后端（QZ_DISPLAY）：
 *   panel — 真面板（默认，由 QZ_PANEL 选描述符）
 *   pbm   — 导出 1bpp 帧供目检（无设备）
 *   none  — 空跑
 */
#include "display.h"

#include <lvgl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display_policy.h"
#include "panel.h"
#include "raster_1bpp.h"

typedef struct {
    const char *name;
    bool dump_frame;   /* 导出 1bpp 帧（PBM） */
    bool dump_raw;     /* 导出 L8 灰度帧（PGM），看阈值化前的样子 */
    bool dry_run;      /* 不落屏 */
} backend_t;

static const backend_t s_backends[] = {
    { "panel", false, false, false },
    { "pbm",   true,  false, true  },
    { "none",  false, false, true  },
};
#define N_BACKENDS ((int)(sizeof(s_backends) / sizeof(s_backends[0])))

static const char *waveform_name(qzos_waveform_t wf)
{
    switch (wf) {
    case QZOS_WF_FULL: return "full";
    case QZOS_WF_FAST: return "fast";
    case QZOS_WF_AUTO:
    default: return "auto";
    }
}

static const backend_t *backend(void)
{
    const char *b = getenv("QZ_DISPLAY");
    if (!b || !*b) return &s_backends[0];
    for (int i = 0; i < N_BACKENDS; i++) {
        if (strcmp(s_backends[i].name, b) == 0) return &s_backends[i];
    }
    fprintf(stderr, "qzos-display: unknown QZ_DISPLAY='%s', using panel\n", b);
    return &s_backends[0];
}

/* ---- 状态 ---- */

static const qzos_panel_t *s_panel;
static uint8_t s_frame[QZOS_FRAME_BYTES];   /* 合成后的 1bpp 帧 */
static uint8_t s_written[QZOS_FRAME_BYTES]; /* 上次真正落屏的帧（差分基准） */
static bool s_have_written;                  /* s_written 是否已装过一次真实帧 */
static qzos_dirty_t s_dirty;                /* 自上次落屏以来的累计脏区 */
static qzos_policy_t s_policy;
static lv_display_t *s_disp;
static bool s_need_commit;                  /* 有新像素待决策 */
static bool s_commit_held;                  /* 开机按住提交，等 shell ready */
static bool s_raw_enabled;
static uint8_t *s_l8_shadow;                /* L8 影子帧，仅 QZ_DUMP_RAW 时分配 */

/* ---- L8 导出（诊断；分配 44992 B，仅在显式请求时） ---- */

static void raw_shadow_blit(const uint8_t *px_map, const lv_area_t *area)
{
    if (!s_l8_shadow) return;
    int32_t aw = area->x2 - area->x1 + 1;
    for (int32_t y = area->y1; y <= area->y2; y++) {
        const uint8_t *row = px_map + (size_t)(y - area->y1) * (size_t)aw;
        memcpy(s_l8_shadow + (size_t)y * s_panel->hor_res + area->x1,
               row, (size_t)aw);
    }
}

static void raw_dump(void)
{
    const char *path = getenv("QZ_DUMP_RAW");
    if (!path || !*path || !s_l8_shadow) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", s_panel->hor_res, s_panel->ver_res);
    fwrite(s_l8_shadow, 1,
           (size_t)s_panel->hor_res * (size_t)s_panel->ver_res, f);
    fclose(f);
}

/* ---- LVGL flush：L8 区域 -> 1bpp 帧 ---- */

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t aw = area->x2 - area->x1 + 1;
    int32_t ah = area->y2 - area->y1 + 1;

    /* L8: 0 = 黑, 255 = 白。阈值 128 —— 1bpp 只能二值化。 */
    (void)qzos_raster_blit_l8(s_frame, s_panel->hor_res, s_panel->ver_res,
                              s_panel->strip_h,
                              px_map, aw, area->x1, area->y1, aw, ah, 128);

    qzos_dirty_add(&s_dirty, area->x1, area->y1, aw, ah);
    s_need_commit = true;

    if (s_raw_enabled) {
        raw_shadow_blit(px_map, area);
        raw_dump();
    }

    lv_display_flush_ready(disp);
}

/* ---- 诊断：把「这次到底哪里变了」打成 ASCII（QZ_DISPLAY_DEBUG） ----
 *
 * 墨水屏上「无缘无故多刷了几次」几乎总是某个控件在做动画或重排，但只看
 * changed 字节数猜不出来源——288B 可以是光标闪一下，也可以是整个按钮重画。
 * 这里把新旧两帧在脏区内的差异逐像素打出来：'#' 本次变黑、'o' 本次变白、
 * '.' 没变。定位这类问题靠的是"看清形状"，不是"数出字节"。
 *
 * 首帧没有"旧帧"可差：s_written 初始化成 0xFF（全黑）只为强制首帧落屏，
 * 拿它当基准会印出一张"整屏刚变白"的假变化图——比不印更糟，因为它看起来
 * 很笃定。所以首帧只说一句"没有基准"，不出图。 */

static void debug_dump_change(const qzos_dirty_t *r)
{
    if (!s_panel || r->empty) return;
    if (!s_have_written) {
        fprintf(stderr, "qzos-display:   change map: (first commit, "
                        "no previous frame to diff against)\n");
        return;
    }
    /* 脏区可能很大（首帧是全屏），全打会淹掉日志：隔行抽样（y += 2），
     * 且总行数封顶 48——要看形状，不需要看全屏每一个像素。 */
    const int32_t x1 = r->x1, x2 = r->x2;
    int32_t rows = 0;
    fprintf(stderr, "qzos-display:   change map (# black, o white, . same):\n");
    for (int32_t y = r->y1; y <= r->y2 && rows < 48; y += 2) {
        /* 缓冲要放得下整个面板宽度（本屏 296），否则首帧那种全屏脏区会被
         * 悄悄截掉一半——半个形状比没有形状更容易误导人。放不下时显式
         * 收尾成 '>'，让读日志的人知道右边还有内容没打出来。 */
        char line[320];
        int n = 0;
        for (int32_t x = x1; x <= x2 && n < (int)sizeof(line) - 2; x++) {
            bool nw = qzos_raster_get_px(s_frame, s_panel->hor_res, s_panel->ver_res,
                                          s_panel->strip_h, x, y, &nw) ? nw : false;
            bool od = false;
            qzos_raster_get_px(s_written, s_panel->hor_res, s_panel->ver_res,
                               s_panel->strip_h, x, y, &od);
            line[n++] = (nw == od) ? '.' : (nw ? '#' : 'o');
        }
        if (x2 - x1 + 1 > (int32_t)sizeof(line) - 2) line[n++] = '>';
        line[n] = '\0';
        fprintf(stderr, "  y=%3d %s\n", y, line);
        rows++;
    }
}

/* ---- 对外接口 ---- */

int qzos_display_init(void)
{
    s_panel = qzos_panel_active();
    uint32_t fbytes = qzos_panel_frame_bytes(s_panel);
    if (fbytes != QZOS_FRAME_BYTES) {
        fprintf(stderr, "qzos-display: panel '%s' frame %u != %u (几何与编译期不符)\n",
                s_panel->name, fbytes, (uint32_t)QZOS_FRAME_BYTES);
        return -1;
    }

    s_disp = lv_display_create(s_panel->hor_res, s_panel->ver_res);
    if (!s_disp) return -1;
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_L8);

    /* 全屏 L8 缓冲：LVGL 的 partial 模式按脏区重绘，但我们要合成完整帧
     * 才能做帧差分与落屏，所以给一整屏。296*152 = 44992 B。 */
    static uint8_t lv_buf[QZOS_FRAME_BYTES * 8];
    lv_display_set_buffers(s_disp, lv_buf, NULL, sizeof(lv_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);

    memset(s_frame, 0, sizeof(s_frame));   /* 0 = 无黑像素 = 全白 */
    memset(s_written, 0xFF, sizeof(s_written)); /* 强制首帧走一次落屏 */
    s_have_written = false;                     /* …但它不是真帧，见 debug_dump_change */
    qzos_dirty_reset(&s_dirty);
    s_need_commit = false;
    /* 开机先按住提交：见 qzos_display_hold 的说明。shell 画出桌面后由宿主
     * 释放（收到 JS 的 {evt:'ready'}），或由兜底定时器释放。 */
    s_commit_held = true;

    const backend_t *be = backend();
    s_raw_enabled = be->dump_raw || (getenv("QZ_DUMP_RAW") != NULL);
    if (s_raw_enabled) {
        s_l8_shadow = malloc((size_t)s_panel->hor_res * (size_t)s_panel->ver_res);
        if (s_l8_shadow) memset(s_l8_shadow, 0xFF, (size_t)s_panel->hor_res * (size_t)s_panel->ver_res);
    }

    qzos_policy_cfg_t cfg;
    qzos_policy_cfg_from_env(&cfg);
    qzos_policy_init(&s_policy, &cfg);

    /* 非落屏后端（pbm/none）不需要设备句柄 */
    if (!be->dry_run) {
        if (qzos_panel_open(s_panel) < 0) {
            fprintf(stderr, "qzos-display: cannot open %s "
                            "(device absent? QZ_DISPLAY=pbm|none)\n",
                    s_panel->dev_path ? s_panel->dev_path : "(none)");
        }
    }

    LV_LOG_INFO("display: %s %dx%d frame=%u backend=%s",
                s_panel->name, s_panel->hor_res, s_panel->ver_res,
                fbytes, be->name);
    return 0;
}

void qzos_display_full_refresh(void)
{
    qzos_policy_request_full(&s_policy);
}

/* 强制一次同步重绘：LVGL 的 lv_timer_handler 不会自行发起重绘，
 * e-ink 宿主必须显式失效化（否则 UI 建好了屏上还是旧的）。 */
void qzos_display_repaint(void)
{
    lv_refr_now(lv_display_get_default());
    lv_obj_invalidate(lv_screen_active());
}

const uint8_t *qzos_display_frame(uint32_t *len)
{
    if (len) *len = (uint32_t)sizeof(s_frame);
    return s_frame;
}

bool qzos_display_dirty_region(int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    if (s_dirty.empty) return false;
    if (x) *x = s_dirty.x1;
    if (y) *y = s_dirty.y1;
    if (w) *w = s_dirty.x2 - s_dirty.x1 + 1;
    if (h) *h = s_dirty.y2 - s_dirty.y1 + 1;
    return true;
}

void qzos_display_commit(void)
{
    /* 开机按住期间不落屏。为什么要它：
     *
     * 首帧提交是**构造上强制**的（s_written 初始化成 0xFF，见 init），所以
     * 开机必然先刷一帧全屏。若 shell 此时还没画完，屏上落的是**空白屏**，
     * 紧接着桌面画好再刷第二帧 —— 两次全刷白费一次波形（e-ink 硬约束：
     * 无事不刷）。
     *
     * 什么时候会变成两次，取决于 shell boot 快慢：Release 构建里 JS 画完
     * 早于第一个 LVGL tick，于是只有一帧；ASan/Debug 构建里慢，就裂成两帧。
     * 实测确认过（ASan 下 boot=2，Release 下 boot=1）。这不只是浪费——它还
     * 让 verify-input.sh 的刷新预算断言在慢机器上**假红**：一个在快机绿、
     * 慢机红的闸门不是闸门。
     *
     * 所以这里按住提交，等 shell 报 ready（或兜底定时器）再放。放行后首帧
     * 携带的就是真正的桌面，恒为一次全刷。
     */
    if (s_commit_held) return;
    if (!s_need_commit) return;
    s_need_commit = false;

    const backend_t *be = backend();
    qzos_dirty_clamp(&s_dirty, s_panel->hor_res, s_panel->ver_res, s_panel->strip_h);

    /* 帧级差分：LVGL 报脏不等于像素真的变了（重绘可能产出相同内容）。
     * e-ink 上「无变化就完全不刷」是硬要求——所以 changed==0 必须一路传到
     * 策略层（策略的契约就是 dirty_pixels==0 -> ACT_NONE），而不是自己
     * 在这里偷偷刷一次。否则每次「重绘了但内容一样」（焦点态切换、样式
     * 重算、LVGL 内部 invalidate）都会白刷一次墨水屏。 */
    int32_t changed = qzos_raster_diff_bytes(s_frame, s_written, sizeof(s_frame));

    /* 脏区取「LVGL 报告的」与「实际变化的」中较窄的一个：
     * 前者可能包含未变像素，后者一定是真的变了。 */
    if (changed == 0) {
        /* 屏上内容与上次提交完全一致：没有可刷的东西。全刷请求要照常
         * 生效（清残影是面板的事，与内容是否变化无关），所以这里只清
         * 脏区，把决策权留给策略的 pending_full 分支。 */
        qzos_dirty_reset(&s_dirty);
    } else {
        qzos_dirty_t real;
        qzos_raster_diff_dirty(s_frame, s_written,
                               s_panel->hor_res, s_panel->ver_res,
                               s_panel->strip_h, &real);
        if (s_dirty.empty || qzos_dirty_pixels(&real) < qzos_dirty_pixels(&s_dirty)) {
            s_dirty = real;
        }
    }

    int32_t total = s_panel->hor_res * s_panel->ver_res;
    qzos_waveform_t wf = QZOS_WF_AUTO;
    qzos_action_t act = qzos_policy_decide(&s_policy, s_panel,
                                           qzos_dirty_pixels(&s_dirty), total, &wf);

    if (be->dump_raw) raw_dump();

    if (act == QZOS_ACT_NONE) {
        /* 没落到屏上：脏区保留，等下一帧合并（避免高频小改动导致频繁刷屏）。
         * changed==0 的情形脏区已经在上面清空了——没有待提交的像素，
         * 留着它只会让后续提交一直以为"有东西要刷"。 */
        if (changed == 0) qzos_dirty_reset(&s_dirty);
        return;
    }

    int rc = 0;

    if (be->dump_frame) {
        uint8_t buf[QZOS_FRAME_BYTES + 64];
        size_t n = qzos_raster_encode_pbm(s_frame, s_panel->hor_res,
                                          s_panel->ver_res, buf, sizeof(buf));
        const char *path = getenv("QZ_PBM");
        if (!path || !*path) path = "/tmp/qzos-frame.pbm";
        FILE *f = fopen(path, "wb");
        if (f) {
            if (n) fwrite(buf, 1, n, f);
            fclose(f);
        } else {
            rc = -1;
        }
    } else if (!be->dry_run) {
        rc = qzos_panel_present(s_panel, s_frame, sizeof(s_frame), wf);
    }

    if (rc == 0) {
        int32_t dirty_px = qzos_dirty_pixels(&s_dirty);
        bool dbg = getenv("QZ_DISPLAY_DEBUG") != NULL;
        qzos_dirty_t rect = s_dirty;   /* reset 之前留一份，给 DEBUG 用 */
        if (dbg) debug_dump_change(&rect);
        memcpy(s_written, s_frame, sizeof(s_frame));
        s_have_written = true;
        qzos_dirty_reset(&s_dirty);
        qzos_policy_on_committed(&s_policy, act);
        /* 刷新统计走 stderr 而不是 LV_LOG_*:lv_conf 把日志等级设为 WARN，
         * 而「这次刷了什么、刷多大」是 e-ink 调参的核心依据，不该被等级屏蔽。
         * 带毫秒时间戳：分辨一次刷新是"动画逐帧来的"还是"一次性状态跳变"只能
         * 看时间分布——两者的 changed 字节数可能一模一样。 */
        fprintf(stderr, "qzos-display: [%6u ms] commit %s wf=%s dirty=%dpx(%.1f%%) "
                        "changed=%dB since_full=%d",
                (unsigned)lv_tick_get(), qzos_action_name(act),
                waveform_name(wf), dirty_px,
                total > 0 ? 100.0 * (double)dirty_px / (double)total : 0.0,
                changed, s_policy.commits_since_full);
        if (dbg)
            fprintf(stderr, " rect x=%d y=%d w=%d h=%d",
                    rect.x1, rect.y1, rect.x2 - rect.x1 + 1,
                    rect.y2 - rect.y1 + 1);
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "qzos-display: commit %s failed (rc=%d)\n",
                qzos_action_name(act), rc);
    }
}

/* ---- 引擎崩溃提示 ----
 *
 * 用 LVGL label 而不是直接往 1bpp 帧里画字：字形来自 Fusion Pixel 位图字体，
 * 走 LVGL 才不用在这里重造一套点阵渲染；而这也顺带保证了「提示自己也要过
 * 显示层」（脏区/波形/提交），不会出现「提示画了但没落屏」。
 *
 * 1bpp 只有黑白两色，所以提示不能靠颜色表达严重程度——它靠**说清下一步**
 * 来表达：还有多久重试。1bpp 屏上动画一律禁止，这里也就没有闪烁。
 */

static lv_obj_t *s_rt_dead_box;

static const char *fmt_retry(char *buf, size_t n, uint32_t retry_ms)
{
    snprintf(buf, n, "Retrying in %u.%us", retry_ms / 1000u,
             (retry_ms % 1000u) / 100u);
    return buf;
}

void qzos_show_rt_dead(uint32_t retry_ms)
{
    if (s_rt_dead_box) {
        /* 已在提示中：只更新倒计时文本，不重建（重建会多一次提交 = 多刷屏） */
        lv_obj_t *t = lv_obj_get_child(s_rt_dead_box, 1);
        if (t) {
            char buf[48];
            lv_label_set_text(t, fmt_retry(buf, sizeof(buf), retry_ms));
        }
        qzos_display_repaint();
        return;
    }

    s_rt_dead_box = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_rt_dead_box);
    lv_obj_set_style_bg_color(s_rt_dead_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_rt_dead_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_rt_dead_box, 1, 0);
    lv_obj_set_style_border_color(s_rt_dead_box, lv_color_black(), 0);
    lv_obj_set_style_radius(s_rt_dead_box, 0, 0);
    lv_obj_set_size(s_rt_dead_box, 292, 80);
    lv_obj_align(s_rt_dead_box, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *t1 = lv_label_create(s_rt_dead_box);
    lv_obj_remove_style_all(t1);
    lv_obj_set_style_text_color(t1, lv_color_black(), 0);
    lv_label_set_text(t1, "JS engine stopped");
    lv_obj_align(t1, LV_ALIGN_TOP_MID, 0, 10);

    char buf[48];
    lv_obj_t *t2 = lv_label_create(s_rt_dead_box);
    lv_obj_remove_style_all(t2);
    lv_obj_set_style_text_color(t2, lv_color_black(), 0);
    lv_label_set_text(t2, fmt_retry(buf, sizeof(buf), retry_ms));
    lv_obj_align(t2, LV_ALIGN_BOTTOM_MID, 0, -10);

    fprintf(stderr, "qzos-display: engine-dead notice shown (%ums)\n", retry_ms);
    qzos_display_repaint();
    qzos_display_full_refresh();   /* 波形切换处，显式全刷保证残影清掉 */
}

void qzos_hide_rt_dead(void)
{
    if (!s_rt_dead_box) return;
    lv_obj_delete(s_rt_dead_box);
    s_rt_dead_box = NULL;
    fprintf(stderr, "qzos-display: engine-dead notice cleared\n");
    qzos_display_repaint();
}

void qzos_display_hold(int on)
{
    s_commit_held = on ? true : false;
    if (!s_commit_held && s_need_commit) {
        /* 放开时立刻提交一次：按住期间 LVGL 一直在画，内容已经就绪。
         * 这里不能等下一个 tick——那会让用户在桌面上多等 33ms 才能看到东西，
         * 而这台设备是墨水屏，多一次提交就多一次波形。 */
        qzos_display_commit();
    }
}

/* shell 起不来时的故障屏。与引擎死亡用同一块框、同一套信息——用户看到的是
 * 同一类现象（屏幕不动、没有解释），区别只在标题行。
 *
 * 这个缺口是实测出来的：QZ_JS_DIR 指到不存在的目录时，兜底定时器会放开提交
 * 并把 LVGL 初始化后的样子（全白）落屏，用户拿到一块**纯白屏 + 零诊断**。
 * 对一台只能靠 USB ADB 救的墨水屏一体机来说，那等于「设备坏了且不知道为什么」。 */
static void notice_box(const char *title, const char *body, bool with_countdown)
{
    s_rt_dead_box = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_rt_dead_box);
    lv_obj_set_style_bg_color(s_rt_dead_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_rt_dead_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_rt_dead_box, 1, 0);
    lv_obj_set_style_border_color(s_rt_dead_box, lv_color_black(), 0);
    lv_obj_set_style_radius(s_rt_dead_box, 0, 0);
    lv_obj_set_size(s_rt_dead_box, 292, with_countdown ? 80 : 90);
    lv_obj_align(s_rt_dead_box, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *t1 = lv_label_create(s_rt_dead_box);
    lv_obj_remove_style_all(t1);
    lv_obj_set_style_text_color(t1, lv_color_black(), 0);
    lv_label_set_text(t1, title);
    lv_obj_align(t1, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *t2 = lv_label_create(s_rt_dead_box);
    lv_obj_remove_style_all(t2);
    lv_obj_set_style_text_color(t2, lv_color_black(), 0);
    char buf[72];
    snprintf(buf, sizeof(buf), "%s", body ? body : "");
    lv_label_set_text(t2, buf);
    if (!with_countdown) {
        /* 错误串可能很长，1bpp 屏上一行放不下。截断要显式（DOT 模式画省略号），
         * 否则用户看不出后面被截了。 */
        lv_obj_set_width(t2, 272);
        lv_label_set_long_mode(t2, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_obj_align(t2, LV_ALIGN_BOTTOM_MID, 0, -12);
}

void qzos_show_boot_failed(const char *msg)
{
    if (!s_rt_dead_box) notice_box("Shell failed to start", msg, false);
    fprintf(stderr, "qzos-display: boot-failed notice shown\n");
    qzos_display_repaint();
    qzos_display_full_refresh();
}
