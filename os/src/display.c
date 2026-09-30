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
static qzos_dirty_t s_dirty;                /* 自上次落屏以来的累计脏区 */
static qzos_policy_t s_policy;
static lv_display_t *s_disp;
static bool s_need_commit;                  /* 有新像素待决策 */
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
    qzos_dirty_reset(&s_dirty);
    s_need_commit = false;

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
        memcpy(s_written, s_frame, sizeof(s_frame));
        qzos_dirty_reset(&s_dirty);
        qzos_policy_on_committed(&s_policy, act);
        /* 刷新统计走 stderr 而不是 LV_LOG_*:lv_conf 把日志等级设为 WARN，
         * 而「这次刷了什么、刷多大」是 e-ink 调参的核心依据，不该被等级屏蔽。 */
        fprintf(stderr, "qzos-display: commit %s wf=%s dirty=%dpx(%.1f%%) "
                        "changed=%dB since_full=%d\n",
                qzos_action_name(act), waveform_name(wf), dirty_px,
                total > 0 ? 100.0 * (double)dirty_px / (double)total : 0.0,
                changed, s_policy.commits_since_full);
    } else {
        fprintf(stderr, "qzos-display: commit %s failed (rc=%d)\n",
                qzos_action_name(act), rc);
    }
}
