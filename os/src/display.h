/*
 * display.h — 显示编排层对外接口（C99）
 *
 * 移植约定：换 SoC / 换屏时只改 panel.c（描述符）或用 QZ_PANEL* 环境变量，
 * 本文件及其调用方不需要改动。
 */
#ifndef QZOS_DISPLAY_H
#define QZOS_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

/* 编译期几何。panel 描述符的几何必须与之一致（init 时校验），
 * 因为 LVGL 缓冲与 qzos-frame.h 的常量按此分配。 */
#define QZOS_HOR_RES 296
#define QZOS_VER_RES 152
#define QZOS_FRAME_BYTES (QZOS_HOR_RES * (QZOS_VER_RES / 8)) /* 5624 */

int  qzos_display_init(void);
void qzos_display_commit(void);

/* 请求一次全刷（开机、清屏、切换波形）。幂等，实际执行在下次 commit。 */
void qzos_display_full_refresh(void);

/* 按住/放开提交。开机时按住，等 shell 画出桌面（收到 {evt:'ready'}）后放开，
 * 让首帧携带真正的桌面——否则开机白刷一帧空白屏（e-ink 硬约束：无事不刷），
 * 且刷新预算断言在慢构建上会假红。放开时若已有待提交内容会立刻提交一次。
 * 兜底：宿主有一个定时器，即使 shell 挂了也会在 N 秒后放开（否则屏一直空白）。 */
void qzos_display_hold(int on);

/* 强制同步重绘。LVGL 的 lv_timer_handler 不会自行发起重绘，
 * e-ink 宿主必须显式失效化，否则 UI 已更新而屏上仍是旧内容。 */
void qzos_display_repaint(void);

/* 读当前合成帧（1bpp，QZOS_FRAME_BYTES 字节）。返回的指针在下次
 * commit 前有效——用于导出、差分校验、外部服务读回。 */
const uint8_t *qzos_display_frame(uint32_t *len);

/* 引擎崩溃期间的整屏提示（"系统正在重启"）。**必须画出来**：「什么都不显示」
 * 正是要修的病症——用户需要一个明确的告知，而不是一块看着正常的死画面。
 * 撤销走 qzos_hide_rt_dead（清屏，重建后由 shell 重画桌面）。 */
void qzos_show_rt_dead(uint32_t retry_ms);
/* shell 启动失败（桌面没起来，用户面对纯白屏）——必须显式告知 */
void qzos_show_boot_failed(const char *msg);
void qzos_hide_rt_dead(void);

/* 当前累计脏区（自上次落屏以来）。无脏区返回 false。 */
bool qzos_display_dirty_region(int32_t *x, int32_t *y, int32_t *w, int32_t *h);

#endif /* QZOS_DISPLAY_H */
