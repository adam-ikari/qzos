/*
 * test_display.c — 纯逻辑层单测：raster_1bpp + display_policy（C99）
 *
 * 这两层不含 I/O、不含全局状态、不依赖 LVGL/uqrpc，因此可以脱离设备跑。
 * 覆盖的是「最难靠目视发现」的部分：条带寻址、掩码移位、边界裁剪、
 * 以及 e-ink 刷新节奏（刷多了伤屏，刷少了积残影）。
 *
 * 构建运行: scripts/test-display.sh
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "display_policy.h"
#include "panel.h"
#include "raster_1bpp.h"

static int g_fail;
static int g_run;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        g_run++;                                                               \
        if (!(cond)) {                                                         \
            g_fail++;                                                          \
            fprintf(stderr, "FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,      \
                    ##__VA_ARGS__);                                           \
        }                                                                      \
    } while (0)

#define H 296
#define V 152
#define SH 8
#define FRAME_BYTES (H * (V / SH))

/* ---- raster: 帧大小与寻址 ---- */

static void test_frame_bytes(void)
{
    CHECK(qzos_raster_frame_bytes(H, V, SH) == FRAME_BYTES,
          "frame bytes = %zu, want %d",
          qzos_raster_frame_bytes(H, V, SH), FRAME_BYTES);
    /* 高度不是条带整数倍 -> 无法寻址，应报错而不是给个错的大小 */
    CHECK(qzos_raster_frame_bytes(H, 150, SH) == 0, "150 rows must be rejected");
    CHECK(qzos_raster_frame_bytes(0, V, SH) == 0, "zero width must be rejected");
}

/* strip8 布局：offset = (y/8)*hor + x, mask = 0x80>>(y%8) */
static void test_set_get_px(void)
{
    static uint8_t f[FRAME_BYTES];
    memset(f, 0, sizeof(f));

    /* strip8 布局的两个要点（写测试时最容易搞反）：
     *   x  -> 直接作为条带内的字节索引（每字节 8 个 x 共享）
     *   y  -> 决定条带号与字节内掩码位 0x80>>(y%strip_h)
     * 所以「一列 8 个 y」= 一个字节的 8 个位。 */

    /* 第 0 带第 0 行 -> offset 0, mask 0x80 */
    qzos_raster_set_px(f, H, V, SH, 0, 0, true);
    CHECK(f[0] == 0x80, "f[0] = 0x%02X, want 0x80", f[0]);

    /* 第 1 带（y=8）第 0 行 -> offset = 1*296 + 5 = 301, mask 0x80 */
    qzos_raster_set_px(f, H, V, SH, 5, 8, true);
    CHECK(f[296 + 5] == 0x80, "f[301] = 0x%02X, want 0x80", f[296 + 5]);

    /* 同一带内不同 y -> 同字节不同掩码位（8 个 y 占满一个字节） */
    qzos_raster_set_px(f, H, V, SH, 5, 9, true);   /* mask 0x40 */
    CHECK(f[296 + 5] == 0xC0, "row9: 0x%02X, want 0xC0", f[296 + 5]);
    qzos_raster_set_px(f, H, V, SH, 5, 15, true);  /* mask 0x01 */
    CHECK(f[296 + 5] == 0xC1, "row15: 0x%02X, want 0xC1", f[296 + 5]);

    /* 末带末行末列: y=151 -> strip 18, offset 18*296+295, mask 0x01 */
    qzos_raster_set_px(f, H, V, SH, 295, 151, true);
    CHECK(f[18 * H + 295] == 0x01, "last px: 0x%02X, want 0x01",
          f[18 * H + 295]);

    /* 读回 */
    bool black = false;
    CHECK(qzos_raster_get_px(f, H, V, SH, 5, 15, &black) && black, "read back");
    /* 清白 */
    qzos_raster_set_px(f, H, V, SH, 5, 15, false);
    CHECK(qzos_raster_get_px(f, H, V, SH, 5, 15, &black) && !black, "cleared");

    /* 越界必须被忽略而非越界写 */
    uint8_t guard[FRAME_BYTES];
    memcpy(guard, f, sizeof(guard));
    qzos_raster_set_px(f, H, V, SH, -1, 0, true);
    qzos_raster_set_px(f, H, V, SH, 0, -1, true);
    qzos_raster_set_px(f, H, V, SH, H, 0, true);
    qzos_raster_set_px(f, H, V, SH, 0, V, true);
    CHECK(memcmp(guard, f, sizeof(guard)) == 0, "out-of-range must not write");
    CHECK(!qzos_raster_get_px(f, H, V, SH, H, 0, &black), "oob read must fail");
}

/* ---- raster: L8 阈值化 ---- */

static void test_blit_l8(void)
{
    static uint8_t f[FRAME_BYTES];
    static uint8_t l8[8 * 8];
    memset(f, 0, sizeof(f));

    /* 8x8 全黑 (0) 与全白 (255) 各一块 */
    for (int i = 0; i < 64; i++) l8[i] = 0;
    int32_t n = qzos_raster_blit_l8(f, H, V, SH, l8, 8, 0, 0, 8, 8, 128);
    CHECK(n == 64, "blit returned %d, want 64", n);
    /* 8x8 黑块 = 8 整字节全 0xFF */
    for (int b = 0; b < 8; b++) {
        CHECK(f[b] == 0xFF, "black block byte %d = 0x%02X", b, f[b]);
    }

    /* 阈值边界：127 -> 黑，128 -> 白（< threshold 判黑） */
    memset(f, 0, sizeof(f));
    for (int i = 0; i < 64; i++) l8[i] = 127;
    (void)qzos_raster_blit_l8(f, H, V, SH, l8, 8, 0, 0, 8, 8, 128);
    CHECK(f[0] == 0xFF, "127 must be black, got 0x%02X", f[0]);
    memset(f, 0, sizeof(f));
    for (int i = 0; i < 64; i++) l8[i] = 128;
    (void)qzos_raster_blit_l8(f, H, V, SH, l8, 8, 0, 0, 8, 8, 128);
    CHECK(f[0] == 0x00, "128 must be white, got 0x%02X", f[0]);

    /* 部分裁剪：8x8 放在 (294,0)，只有 x=294,295 两列在屏内。
     * 布局要点：x 是字节索引（每字节 8 个 x 共享），掩码由 y 决定。
     * 2 列 x 8 行 -> 2 字节，每字节 8 个 y 全黑 = 0xFF。 */
    memset(f, 0, sizeof(f));
    for (int i = 0; i < 64; i++) l8[i] = 0;
    n = qzos_raster_blit_l8(f, H, V, SH, l8, 8, 294, 0, 8, 8, 128);
    CHECK(n == 2 * 8, "clipped blit returned %d, want 16", n);
    CHECK(f[294] == 0xFF && f[295] == 0xFF,
          "clipped: bytes 294/295 = 0x%02X/0x%02X, want 0xFF/0xFF",
          f[294], f[295]);
    CHECK(f[293] == 0x00, "byte 293 must stay white, got 0x%02X", f[293]);
    CHECK(f[296] == 0x00, "byte 296 must stay white, got 0x%02X", f[296]);

    /* 完全越界 */
    n = qzos_raster_blit_l8(f, H, V, SH, l8, 8, 400, 0, 8, 8, 128);
    CHECK(n == 0, "fully-oob blit returned %d, want 0", n);
}

/* ---- raster: 脏区与差分 ---- */

static void test_dirty(void)
{
    qzos_dirty_t d;
    qzos_dirty_reset(&d);
    CHECK(d.empty, "fresh dirty is empty");
    CHECK(qzos_dirty_pixels(&d) == 0, "fresh dirty pixels = 0");

    /* 扩张成包围盒 */
    qzos_dirty_add(&d, 10, 10, 5, 5);
    qzos_dirty_add(&d, 50, 30, 5, 5);
    CHECK(!d.empty, "not empty after add");
    CHECK(d.x1 == 10 && d.y1 == 10 && d.x2 == 54 && d.y2 == 34,
          "bbox = (%d,%d)-(%d,%d)", d.x1, d.y1, d.x2, d.y2);

    /* 条带对齐：y=10..14 -> 8..15；y=30..34 -> 24..39 */
    qzos_dirty_clamp(&d, H, V, SH);
    CHECK(d.y1 == 8, "y1 = %d, want 8 (strip aligned)", d.y1);
    CHECK(d.y2 == 39, "y2 = %d, want 39 (strip aligned)", d.y2);

    /* 裁剪到屏内 */
    qzos_dirty_reset(&d);
    qzos_dirty_add(&d, 290, 148, 20, 20);
    qzos_dirty_clamp(&d, H, V, SH);
    CHECK(d.x2 == 295, "x2 = %d, want 295", d.x2);
    CHECK(d.y2 == 151, "y2 = %d, want 151", d.y2);
    CHECK(d.x1 == 290 && d.y1 == 144, "clamp x1/y1 = %d/%d", d.x1, d.y1);

    /* 空操作参数 */
    qzos_dirty_reset(&d);
    qzos_dirty_add(&d, 0, 0, 0, 10);
    qzos_dirty_add(&d, 0, 0, 10, -1);
    CHECK(d.empty, "zero-size adds must be ignored");
}

static void test_diff(void)
{
    static uint8_t a[FRAME_BYTES];
    static uint8_t b[FRAME_BYTES];
    memset(a, 0, sizeof(a));
    memcpy(b, a, sizeof(b));

    CHECK(qzos_raster_diff_bytes(a, b, sizeof(a)) == 0, "identical frames");

    /* 只改第 3 带（y=24..31）的第一个字节 */
    b[3 * H] ^= 0xFF;
    CHECK(qzos_raster_diff_bytes(a, b, sizeof(a)) == 1, "one byte differs");

    qzos_dirty_t d;
    qzos_raster_diff_dirty(a, b, H, V, SH, &d);
    CHECK(!d.empty, "diff found region");
    CHECK(d.y1 == 24 && d.y2 == 31, "dirty strip y = %d..%d, want 24..31", d.y1, d.y2);
    /* 整条带 296 像素宽（当前实现取整条带，够用且更省） */
    CHECK(d.x1 == 0 && d.x2 == 295, "dirty x = %d..%d", d.x1, d.x2);
    CHECK(qzos_dirty_pixels(&d) == 296 * 8, "dirty pixels = %d, want %d",
          qzos_dirty_pixels(&d), 296 * 8);

    /* 改两条带 -> 脏区覆盖两带 */
    b[7 * H] ^= 0xFF;
    qzos_raster_diff_dirty(a, b, H, V, SH, &d);
    CHECK(d.y1 == 24 && d.y2 == 63, "two strips y = %d..%d, want 24..63", d.y1, d.y2);

    /* 无差异 -> 空 */
    memcpy(b, a, sizeof(b));
    qzos_raster_diff_dirty(a, b, H, V, SH, &d);
    CHECK(d.empty, "no diff -> empty region");
}

/* ---- raster: PBM 编码 ---- */

static void test_encode_pbm(void)
{
    static uint8_t f[FRAME_BYTES];
    static uint8_t out[FRAME_BYTES + 64];
    memset(f, 0, sizeof(f));

    /* 全白帧 */
    size_t n = qzos_raster_encode_pbm(f, H, V, out, sizeof(out));
    CHECK(n > 0, "encode white frame");
    CHECK(memcmp(out, "P4\n296 152\n", 11) == 0, "PBM header");
    /* 11 字节头 + 37*152 行 */
    CHECK(n == 11 + 37 * 152, "pbm len = %zu, want %d", n, 11 + 37 * 152);
    for (size_t i = 11; i < n; i++) {
        if (out[i] != 0) { CHECK(0, "white frame byte %zu = 0x%02X", i, out[i]); break; }
    }

    /* 左上角 8x8 黑块（y=0..7）-> PBM 第 0..7 行首字节 0xFF，第 8 行白 */
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) qzos_raster_set_px(f, H, V, SH, x, y, true);
    }
    n = qzos_raster_encode_pbm(f, H, V, out, sizeof(out));
    CHECK(n > 0 && out[11] == 0xFF, "pbm row0 = 0x%02X, want 0xFF", out[11]);
    /* y=1 同在黑块内（条带 0 覆盖 y=0..7），所以第 1 行首字节也是黑。
     * 这正是 strip8 布局在 PBM 导出时必须逐行重新打包的原因——
     * 条带内 8 行共用同一份 offset 数据。 */
    CHECK(out[11 + 37] == 0xFF, "pbm row1 (y=1, in block) = 0x%02X, want 0xFF",
          out[11 + 37]);
    CHECK(out[11 + 7 * 37] == 0xFF, "pbm row7 (y=7, in block) = 0x%02X, want 0xFF",
          out[11 + 7 * 37]);
    CHECK(out[11 + 8 * 37] == 0x00, "pbm row8 (y=8, past block) = 0x%02X, want 0x00",
          out[11 + 8 * 37]);

    /* 缓冲不足 -> 返回 0，不越界写 */
    CHECK(qzos_raster_encode_pbm(f, H, V, out, 16) == 0, "small buffer must fail");
}

/* ---- policy: 刷新节奏 ---- */

static qzos_panel_t make_panel(bool cap_full, bool cap_wave)
{
    qzos_panel_t p;
    memset(&p, 0, sizeof(p));
    p.name = "test";
    p.hor_res = H;
    p.ver_res = V;
    p.strip_h = SH;
    p.layout = QZOS_FRAME_STRIP8;
    p.cap_dirty = true;
    p.cap_full_refresh = cap_full;
    p.cap_waveform_select = cap_wave;
    return p;
}

static void test_policy_basic(void)
{
    qzos_panel_t panel = make_panel(true, false);
    qzos_policy_cfg_t cfg = { 3, 0, true }; /* 每 3 次提交全刷一次 */
    qzos_policy_t pol;
    qzos_policy_init(&pol, &cfg);
    int total = H * V;
    qzos_waveform_t wf = QZOS_WF_AUTO;

    /* 开机首帧必须全刷（屏上有旧系统残留） */
    CHECK(pol.pending_full, "boot requests full");
    qzos_action_t a = qzos_policy_decide(&pol, &panel, total, total, &wf);
    CHECK(a == QZOS_ACT_FULL, "boot action = %s, want full", qzos_action_name(a));
    CHECK(wf == QZOS_WF_FULL, "boot waveform = %d, want FULL", (int)wf);
    qzos_policy_on_committed(&pol, a);
    CHECK(pol.commits_total == 1 && pol.fulls_total == 1, "boot counted");
    CHECK(!pol.pending_full, "pending cleared");

    /* 无变化 -> 完全不动屏（e-ink 寿命要求） */
    a = qzos_policy_decide(&pol, &panel, 0, total, &wf);
    CHECK(a == QZOS_ACT_NONE, "no-dirty action = %s, want none", qzos_action_name(a));

    /* 局部改动 -> 局部刷新 */
    a = qzos_policy_decide(&pol, &panel, 296 * 8, total, &wf);
    CHECK(a == QZOS_ACT_FAST, "small change = %s, want fast", qzos_action_name(a));
    qzos_policy_on_committed(&pol, a);

    /* 第二次局部：还没到周期（since_full=1, 需要 >=3） */
    a = qzos_policy_decide(&pol, &panel, 296 * 8, total, &wf);
    CHECK(a == QZOS_ACT_FAST, "2nd change = %s, want fast", qzos_action_name(a));
    qzos_policy_on_committed(&pol, a);

    /* 第三次：since_full=2, +1 >= 3 -> 全刷清残影 */
    a = qzos_policy_decide(&pol, &panel, 296 * 8, total, &wf);
    CHECK(a == QZOS_ACT_FULL, "3rd change = %s, want full", qzos_action_name(a));
    qzos_policy_on_committed(&pol, a);
    CHECK(pol.commits_since_full == 0, "counter reset after full");
    CHECK(pol.fulls_total == 2, "fulls = %d, want 2", pol.fulls_total);
}

static void test_policy_edges(void)
{
    int total = H * V;
    qzos_waveform_t wf = QZOS_WF_AUTO;

    /* 显式请求全刷随时生效 */
    {
        qzos_panel_t panel = make_panel(true, false);
        qzos_policy_t pol;
        qzos_policy_init(&pol, NULL);
        qzos_policy_on_committed(&pol, QZOS_ACT_FAST); /* 清掉开机 pending */
        qzos_policy_request_full(&pol);
        CHECK(qzos_policy_decide(&pol, &panel, 100, total, &wf) == QZOS_ACT_FULL,
              "explicit full must win");
    }

    /* 面板不支持全刷 -> 退化为局部，不假装全刷 */
    {
        qzos_panel_t panel = make_panel(false, false);
        qzos_policy_t pol;
        qzos_policy_init(&pol, NULL);
        CHECK(qzos_policy_decide(&pol, &panel, total, total, &wf) == QZOS_ACT_FAST,
              "no-full-refresh panel must degrade to fast");
    }

    /* full_every=0 -> 永不做周期全刷，只在显式请求时 */
    {
        qzos_panel_t panel = make_panel(true, false);
        qzos_policy_cfg_t cfg = { 0, 0, true };
        qzos_policy_t pol;
        qzos_policy_init(&pol, &cfg);
        qzos_policy_on_committed(&pol, QZOS_ACT_FAST);
        for (int i = 0; i < 50; i++) {
            CHECK(qzos_policy_decide(&pol, &panel, 100, total, &wf) == QZOS_ACT_FAST,
                  "full_every=0 must not full-refresh (i=%d)", i);
            qzos_policy_on_committed(&pol, QZOS_ACT_FAST);
        }
        CHECK(pol.fulls_total == 0, "no periodic fulls, got %d", pol.fulls_total);
    }

    /* 快速波形：脏区小于阈值才走 */
    {
        qzos_panel_t panel = make_panel(true, true);
        qzos_policy_cfg_t cfg = { 0, 10, true }; /* 脏区 <=10% 走 fast 波形 */
        qzos_policy_t pol;
        qzos_policy_init(&pol, &cfg);
        qzos_policy_on_committed(&pol, QZOS_ACT_FAST);
        /* 5% 脏区 -> 显式要求快速波形（省时收益明确） */
        qzos_action_t a = qzos_policy_decide(&pol, &panel, total / 20, total, &wf);
        CHECK(a == QZOS_ACT_FAST, "5%% -> fast");
        CHECK(wf == QZOS_WF_FAST, "5%% -> waveform FAST, got %d", (int)wf);
        /* 50% 脏区 -> 仍是局部更新，但波形交回驱动（AUTO）：
         * 强制快速波形此时省不了多少时间，残影风险却一样 */
        a = qzos_policy_decide(&pol, &panel, total / 2, total, &wf);
        CHECK(a == QZOS_ACT_FAST, "50%% -> still fast action");
        CHECK(wf == QZOS_WF_AUTO,
              "50%% -> waveform AUTO (not worth forcing fast), got %d", (int)wf);
    }

    /* auto_commit=0 -> 只在显式请求时刷 */
    {
        qzos_panel_t panel = make_panel(true, false);
        qzos_policy_cfg_t cfg = { 5, 0, false };
        qzos_policy_t pol;
        qzos_policy_init(&pol, &cfg);
        qzos_policy_on_committed(&pol, QZOS_ACT_FAST); /* 清 pending */
        CHECK(qzos_policy_decide(&pol, &panel, total, total, &wf) == QZOS_ACT_NONE,
              "auto_commit=0 must not commit on dirty");
        qzos_policy_request_full(&pol);
        CHECK(qzos_policy_decide(&pol, &panel, 0, total, &wf) == QZOS_ACT_FULL,
              "explicit full works even with auto_commit=0");
    }

    /* NONE 动作不计入提交统计 */
    {
        qzos_policy_t pol;
        qzos_policy_init(&pol, NULL);
        qzos_policy_on_committed(&pol, QZOS_ACT_FAST);
        int before = pol.commits_total;
        qzos_policy_on_committed(&pol, QZOS_ACT_NONE);
        CHECK(pol.commits_total == before, "NONE must not count as commit");
    }
}

/* ---- panel: 描述符与帧大小 ---- */

static void test_panel(void)
{
    const qzos_panel_t *p = qzos_panel_by_name("mp-d261");
    CHECK(p != NULL, "mp-d261 descriptor exists");
    if (p) {
        CHECK(p->hor_res == H && p->ver_res == V, "mp-d261 geometry");
        CHECK(qzos_panel_frame_bytes(p) == FRAME_BYTES,
              "mp-d261 frame = %u, want %d", qzos_panel_frame_bytes(p), FRAME_BYTES);
        CHECK(p->layout == QZOS_FRAME_STRIP8, "mp-d261 layout is strip8");
    }
    CHECK(qzos_panel_by_name("nonexistent-panel") == NULL, "unknown name -> NULL");
    CHECK(qzos_panel_default() != NULL, "default panel always resolves");

    /* LINEAR 布局帧大小 = (hor/8)*ver，与 STRIP8 不同 */
    qzos_panel_t lin = make_panel(false, false);
    lin.layout = QZOS_FRAME_LINEAR;
    CHECK(qzos_panel_frame_bytes(&lin) == (uint32_t)((H / 8) * V),
          "linear frame = %u", qzos_panel_frame_bytes(&lin));

    /* 遍历内置表 */
    int idx = 0, n = 0;
    while (qzos_panel_builtin(&idx) != NULL) n++;
    CHECK(n >= 1, "builtin panel table not empty (n=%d)", n);
}

int main(void)
{
    test_frame_bytes();
    test_set_get_px();
    test_blit_l8();
    test_dirty();
    test_diff();
    test_encode_pbm();
    test_policy_basic();
    test_policy_edges();
    test_panel();

    printf("%s: %d checks, %d failed\n",
           g_fail ? "FAILED" : "OK", g_run, g_fail);
    return g_fail ? 1 : 0;
}
