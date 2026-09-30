/*
 * raster_1bpp.c — 1bpp 帧缓冲栅格器（C99，无状态纯函数）
 *
 * 纯位运算 + 整数，无平台假设、无全局状态、无 I/O。条带高度由调用方传入
 * （来自 panel 描述符），因此这一层对具体屏完全无感——换 SoC 不用改这里。
 *
 * 帧布局（STRIP8，与 C1Terminal internal/terminalui/frame.go 一致）：
 *     offset = (y/strip_h)*hor_res + x
 *     mask   = 0x80 >> (y % strip_h)
 */
#include "raster_1bpp.h"

#include <stdio.h>
#include <string.h>

/* ---- 脏区 ---- */

void qzos_dirty_reset(qzos_dirty_t *d)
{
    if (!d) return;
    d->x1 = 0; d->y1 = 0; d->x2 = -1; d->y2 = -1;
    d->empty = true;
}

void qzos_dirty_add(qzos_dirty_t *d, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!d || w <= 0 || h <= 0) return;
    int32_t nx2 = x + w - 1;
    int32_t ny2 = y + h - 1;
    if (d->empty) {
        d->x1 = x; d->y1 = y; d->x2 = nx2; d->y2 = ny2;
        d->empty = false;
        return;
    }
    if (x < d->x1) d->x1 = x;
    if (y < d->y1) d->y1 = y;
    if (nx2 > d->x2) d->x2 = nx2;
    if (ny2 > d->y2) d->y2 = ny2;
}

void qzos_dirty_add_all(qzos_dirty_t *d, int32_t hor_res, int32_t ver_res)
{
    if (!d || hor_res <= 0 || ver_res <= 0) return;
    qzos_dirty_add(d, 0, 0, hor_res, ver_res);
}

void qzos_dirty_clamp(qzos_dirty_t *d, int32_t hor_res, int32_t ver_res,
                      int32_t strip_h)
{
    if (!d || d->empty) return;
    if (strip_h <= 0) strip_h = 1;

    /* 裁剪到屏幕内 */
    if (d->x1 < 0) d->x1 = 0;
    if (d->y1 < 0) d->y1 = 0;
    if (d->x2 > hor_res - 1) d->x2 = hor_res - 1;
    if (d->y2 > ver_res - 1) d->y2 = ver_res - 1;

    /* 扩张到条带边界：半条带无法独立寻址 */
    d->y1 -= d->y1 % strip_h;
    d->y2 += strip_h - 1 - (d->y2 % strip_h);

    if (d->x2 > hor_res - 1) d->x2 = hor_res - 1;
    if (d->y2 > ver_res - 1) d->y2 = ver_res - 1;

    if (d->x1 > d->x2 || d->y1 > d->y2) qzos_dirty_reset(d);
}

int32_t qzos_dirty_pixels(const qzos_dirty_t *d)
{
    if (!d || d->empty) return 0;
    return (d->x2 - d->x1 + 1) * (d->y2 - d->y1 + 1);
}

/* ---- 几何 ---- */

size_t qzos_raster_frame_bytes(int32_t hor_res, int32_t ver_res, int32_t strip_h)
{
    if (hor_res <= 0 || ver_res <= 0 || strip_h <= 0) return 0;
    if (ver_res % strip_h != 0) return 0;
    return (size_t)hor_res * (size_t)(ver_res / strip_h);
}

/* ---- 单像素 ---- */

void qzos_raster_set_px(uint8_t *frame, int32_t hor_res, int32_t ver_res,
                        int32_t strip_h,
                        int32_t x, int32_t y, bool black)
{
    if (!frame || strip_h <= 0) return;
    if (x < 0 || x >= hor_res || y < 0 || y >= ver_res) return;
    size_t off = (size_t)(y / strip_h) * (size_t)hor_res + (size_t)x;
    uint8_t mask = (uint8_t)(0x80u >> (y % strip_h));
    if (black) frame[off] |= mask;
    else       frame[off] = (uint8_t)(frame[off] & (uint8_t)~mask);
}

bool qzos_raster_get_px(const uint8_t *frame, int32_t hor_res, int32_t ver_res,
                        int32_t strip_h,
                        int32_t x, int32_t y, bool *out_black)
{
    if (!frame || !out_black || strip_h <= 0) return false;
    if (x < 0 || x >= hor_res || y < 0 || y >= ver_res) return false;
    size_t off = (size_t)(y / strip_h) * (size_t)hor_res + (size_t)x;
    uint8_t mask = (uint8_t)(0x80u >> (y % strip_h));
    *out_black = (frame[off] & mask) != 0;
    return true;
}

/* ---- L8 矩形 -> 1bpp ---- */

int32_t qzos_raster_blit_l8(uint8_t *frame, int32_t hor_res, int32_t ver_res,
                            int32_t strip_h,
                            const uint8_t *src, int32_t src_w,
                            int32_t x, int32_t y, int32_t w, int32_t h,
                            uint8_t threshold)
{
    if (!frame || !src || w <= 0 || h <= 0) return 0;

    /* 负偏移时源指针要相应前移，裁掉屏幕外的部分 */
    int32_t skip_x = (x < 0) ? -x : 0;
    int32_t skip_y = (y < 0) ? -y : 0;
    int32_t x0 = (x < 0) ? 0 : x;
    int32_t y0 = (y < 0) ? 0 : y;
    int32_t x1 = x + w;
    int32_t y1 = y + h;
    if (x1 > hor_res) x1 = hor_res;
    if (y1 > ver_res) y1 = ver_res;
    if (x0 >= x1 || y0 >= y1) return 0;

    for (int32_t yy = y0; yy < y1; yy++) {
        const uint8_t *row = src + (size_t)(yy - y0 + skip_y) * (size_t)src_w;
        for (int32_t xx = x0; xx < x1; xx++) {
            uint8_t v = row[xx - x0 + skip_x];
            qzos_raster_set_px(frame, hor_res, ver_res, strip_h,
                               xx, yy, v < threshold);
        }
    }
    return (x1 - x0) * (y1 - y0);
}

/* ---- 差分 ---- */

int32_t qzos_raster_diff_bytes(const uint8_t *a, const uint8_t *b, size_t n)
{
    if (!a || !b) return 0;
    int32_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) diff++;
    }
    return diff;
}

void qzos_raster_diff_dirty(const uint8_t *a, const uint8_t *b,
                            int32_t hor_res, int32_t ver_res, int32_t strip_h,
                            qzos_dirty_t *out)
{
    if (!out) return;
    qzos_dirty_reset(out);
    if (!a || !b || hor_res <= 0 || ver_res <= 0 || strip_h <= 0) return;
    if (ver_res % strip_h != 0) return;

    /* 逐条带比对：整条带相同就跳过。这是局部刷新的主要收益点——
     * 典型 UI 改动只涉及 1~2 条带（8~16 行），而非整屏 19 条带。 */
    int32_t strips = ver_res / strip_h;
    for (int32_t s = 0; s < strips; s++) {
        const uint8_t *ra = a + (size_t)s * (size_t)hor_res;
        const uint8_t *rb = b + (size_t)s * (size_t)hor_res;
        if (memcmp(ra, rb, (size_t)hor_res) == 0) continue;
        qzos_dirty_add(out, 0, s * strip_h, hor_res, strip_h);
    }
    qzos_dirty_clamp(out, hor_res, ver_res, strip_h);
}

/* ---- PBM 编码（P4） ---- */

size_t qzos_raster_encode_pbm(const uint8_t *frame, int32_t hor_res, int32_t ver_res,
                              uint8_t *out, size_t out_cap)
{
    /* 条带高度固定为 8：PBM 导出口径是固定的 296x152 面板 */
    const int32_t sh = 8;
    if (!frame || !out) return 0;
    if (hor_res <= 0 || ver_res <= 0) return 0;
    if (ver_res % sh != 0) return 0;

    int row_bytes = (hor_res + 7) / 8;
    char header[64];
    int hn = snprintf(header, sizeof(header), "P4\n%d %d\n", hor_res, ver_res);
    if (hn < 0) return 0;
    size_t need = (size_t)hn + (size_t)row_bytes * (size_t)ver_res;
    if (need > out_cap) return 0;

    size_t pos = 0;
    memcpy(out + pos, header, (size_t)hn);
    pos += (size_t)hn;

    for (int32_t y = 0; y < ver_res; y++) {
        uint8_t *row = out + pos;
        for (int32_t bx = 0; bx < row_bytes; bx++) {
            uint8_t b = 0;
            for (int bit = 0; bit < 8; bit++) {
                int32_t x = bx * 8 + bit;
                if (x >= hor_res) break;
                bool black = false;
                (void)qzos_raster_get_px(frame, hor_res, ver_res, sh, x, y, &black);
                if (black) b |= (uint8_t)(0x80u >> bit);
            }
            row[bx] = b;
        }
        pos += (size_t)row_bytes;
    }
    return pos;
}
