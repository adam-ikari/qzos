/*
 * panel.h — 面板描述符与后端接口（可移植性的关键层）
 *
 * 移植到其他 SoC 时，只需要改这一层：新增一个 qzos_panel_t 描述符
 * （几何、帧布局、设备路径、刷新方式），其余各层完全不感知具体硬件。
 *
 * 已知的两种 1bpp 帧布局（本项目只用第一种，但接口不排斥第二种）：
 *   STRIP8  纵向条带，每 8 行一条带，带内每像素 1 bit
 *           offset = (y/8)*hor_res + x, mask = 0x80>>(y%8)
 *           —— MP-D261 / C1 Slim
 *   LINEAR  横向连续，每行 hor_res 字节，每字节 8 像素
 *           offset = y*(hor_res/8) + x/8, mask = 0x80>>(x%8)
 *           —— 常见于 SPI 直连控制器
 *
 * 刷新方式（waveform）：
 *   WF_AUTO   驱动/控制器自己判断（写 refresh sysfs 触发）
 *   WF_FULL   显式全刷，清 e-ink 残影
 *   WF_FAST   仅局部波形，快但积残影
 */
#ifndef QZOS_PANEL_H
#define QZOS_PANEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- 帧布局 ---- */

/* 条带行数：STRIP8 布局下帧按每 8 行一条带寻址，这是该布局的固定参数，
 * 不是可调项（换布局而非改这个数）。 */
#define QZOS_STRIP_H 8

typedef enum {
    QZOS_FRAME_STRIP8 = 0, /* 纵向条带，MP-D261 */
    QZOS_FRAME_LINEAR  = 1 /* 横向连续 */
} qzos_frame_layout_t;

/* ---- 刷新波形 ---- */
typedef enum {
    QZOS_WF_AUTO = 0,
    QZOS_WF_FULL = 1,
    QZOS_WF_FAST = 2
} qzos_waveform_t;

/* ---- 面板描述符（全部为静态常量，运行时只读） ---- */
typedef struct {
    const char *name;             /* "mp-d261" 等，QZ_PANEL 选择用 */
    int32_t hor_res;
    int32_t ver_res;
    int32_t strip_h;              /* 条带行数，仅 STRIP8 有意义 */
    qzos_frame_layout_t layout;

    const char *dev_path;         /* 帧设备，如 /dev/epaper_lcd；NULL = 无帧设备 */
    const char *refresh_path;     /* 全刷触发 sysfs；NULL = 无此机制 */
    const char *waveform_path;    /* 波形/快速刷新选择 sysfs；NULL = 无 */

    bool dev_writable_frame;      /* 帧设备是否接受整帧 write（false = 只出图不落屏） */
    uint32_t frame_bytes;         /* 0 = 由几何推导 */

    /* 能力位：决定策略层能做什么 */
    bool cap_dirty;               /* 支持按脏区决策（当前驱动整帧写，策略仍可省提交） */
    bool cap_full_refresh;        /* 支持全刷清残影 */
    bool cap_waveform_select;     /* 支持切换波形 */
} qzos_panel_t;

/* 整帧字节数：优先用 desc->frame_bytes，否则按几何与布局推导 */
uint32_t qzos_panel_frame_bytes(const qzos_panel_t *p);

/* ---- 面板注册表 ---- */
/* 按名字取面板描述符；未知名字回退到内置默认（保证宿主总能起来）。
 * 环境变量 QZ_PANEL 可覆盖默认选择。 */
const qzos_panel_t *qzos_panel_by_name(const char *name);
const qzos_panel_t *qzos_panel_default(void);
/* 遍历内置描述符（调试用；*idx 传入 0 起，返回 NULL 表示结束） */
const qzos_panel_t *qzos_panel_builtin(int *idx);

/* ---- 后端 I/O（唯一碰设备的地方） ---- */
/* 把整帧送到面板。waveform 决定是否触发全刷。
 * 返回 0 成功，非 0 失败（errno 语义）。 */
int qzos_panel_present(const qzos_panel_t *p, const uint8_t *frame,
                       uint32_t len, qzos_waveform_t wf);

/* 尝试打开帧设备（可缓存 fd；失败返回 -1 不致命，宿主继续跑） */
int qzos_panel_open(const qzos_panel_t *p);
void qzos_panel_close(void);
/* 生效的面板描述符 = qzos_panel_default() + QZ_PANEL_DEV/REFRESH/WAVEFORM
 * 环境覆盖。移植到别的屏时优先用这个（其余 API 接受任意描述符）。 */
const qzos_panel_t *qzos_panel_active(void);

/* 面板是否真的能落屏（有设备且打开成功） */
bool qzos_panel_available(void);

#endif /* QZOS_PANEL_H */
