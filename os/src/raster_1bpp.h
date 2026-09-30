/*
 * raster_1bpp.h — 1bpp 帧缓冲栅格器（纯逻辑，零状态、零 I/O、C99）
 *
 * 帧布局（C1 Slim / MP-D261，与 C1Terminal internal/terminalui/frame.go 一致）：
 *     offset = (y/8)*hor_res + x      每条带 8 行，带内每像素 1 bit
 *     mask   = 0x80 >> (y%8)         置位 = 黑
 *     整帧   = hor_res * (ver_res/8) 字节（296x152 -> 5624）
 *
 * 驱动只接受整帧 write()，没有区域参数——所以「局部刷新」不是「少写字节」，
 * 而是：用 dirty 区域判断是否值得刷、是否需要全刷波形清残影。这一层因此
 * 额外提供脏区追踪（对齐到条带边界）。
 *
 * 单独拆出来的理由：带偏移、掩码移位、边界裁剪、条带对齐是这类代码最容易
 * 错且最难靠目视发现的地方，必须能脱离设备与 LVGL 单测。
 */
#ifndef QZOS_RASTER_1BPP_H
#define QZOS_RASTER_1BPP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 脏区：闭区间 [x1,x2] x [y1,y2]；empty 为真表示无脏像素。
 * clamp 之后 y 对齐到条带边界（y1 % strip_h == 0，y2 % strip_h == strip_h-1），
 * 因为帧按条带寻址，半条带无法独立定位。 */
typedef struct {
    int32_t x1, y1, x2, y2;
    bool empty;
} qzos_dirty_t;

void qzos_dirty_reset(qzos_dirty_t *d);
void qzos_dirty_add(qzos_dirty_t *d, int32_t x, int32_t y, int32_t w, int32_t h);
void qzos_dirty_add_all(qzos_dirty_t *d, int32_t hor_res, int32_t ver_res);
/* 把 y 扩张到条带边界，并裁剪到屏幕内 */
void qzos_dirty_clamp(qzos_dirty_t *d, int32_t hor_res, int32_t ver_res,
                      int32_t strip_h);
int32_t qzos_dirty_pixels(const qzos_dirty_t *d);

/* 整帧字节数（要求 hor_res > 0、ver_res 为 strip_h 的倍数） */
size_t qzos_raster_frame_bytes(int32_t hor_res, int32_t ver_res, int32_t strip_h);

/* 单像素写入；越界自动忽略（调用方不必自己判边界） */
void qzos_raster_set_px(uint8_t *frame, int32_t hor_res, int32_t ver_res,
                        int32_t strip_h,
                        int32_t x, int32_t y, bool black);

/* 读单像素；越界返回 false */
bool qzos_raster_get_px(const uint8_t *frame, int32_t hor_res, int32_t ver_res,
                        int32_t strip_h,
                        int32_t x, int32_t y, bool *out_black);

/* 用 src 覆盖 dst 中 (x,y,w,h) 矩形对应的条带比特。src 是 L8 灰度
 * （0=黑 255=白），按 threshold 阈值化：v < threshold -> 黑。
 * 越界部分自动裁剪。返回实际写入的像素数，0 表示完全越界。 */
int32_t qzos_raster_blit_l8(uint8_t *frame, int32_t hor_res, int32_t ver_res,
                            int32_t strip_h,
                            const uint8_t *src, int32_t src_w,
                            int32_t x, int32_t y, int32_t w, int32_t h,
                            uint8_t threshold);

/* 帧差分：返回不同的字节数，0 表示两帧一致。 */
int32_t qzos_raster_diff_bytes(const uint8_t *a, const uint8_t *b, size_t n);

/* 把 a、b 两帧的差异并入脏区（逐条带比对，跳过整段相同的条带）。
 * strip_h 决定跳过的粒度：8 行一次比较，典型 UI 改动只涉及 1~2 条带。 */
void qzos_raster_diff_dirty(const uint8_t *a, const uint8_t *b,
                            int32_t hor_res, int32_t ver_res, int32_t strip_h,
                            qzos_dirty_t *out);

/* 编码为 PBM（P4）字节流，写入调用方缓冲。返回写入长度，0 = 缓冲不足。
 * 纯编码，不碰文件——落盘是 panel 层的事。 */
size_t qzos_raster_encode_pbm(const uint8_t *frame, int32_t hor_res, int32_t ver_res,
                              uint8_t *out, size_t out_cap);

#endif /* QZOS_RASTER_1BPP_H */
