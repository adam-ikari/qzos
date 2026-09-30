/*
 * panel.c — 面板描述符注册表 + 设备 I/O（C99）
 *
 * 整个项目里唯一知道「MP-D261 的设备节点长什么样」的文件。
 * 移植到其他 SoC：在 s_builtin[] 加一条描述符，或运行时用 QZ_PANEL /
 * QZ_PANEL_DEV / QZ_PANEL_REFRESH 覆盖，无需改动其余各层。
 */
#include "panel.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- 内置面板描述符 ---- */

static const qzos_panel_t s_builtin[] = {
    {
        /* 快易典 C1 Slim / MP-D261，Ingenic XBurst。
         * 驱动只接受整帧 write(5624)，没有区域参数；全刷靠写 refresh sysfs。 */
        .name = "mp-d261",
        .hor_res = 296,
        .ver_res = 152,
        .strip_h = QZOS_STRIP_H,
        .layout = QZOS_FRAME_STRIP8,
        .dev_path = "/dev/epaper_lcd",
        .refresh_path = "/sys/devices/platform/e0266a128/epaper/refresh",
        .waveform_path = "/sys/devices/platform/e0266a128/epaper/fast_refresh_only",
        .dev_writable_frame = true,
        .frame_bytes = 0, /* 由几何推导：296 * 19 = 5624 */
        .cap_dirty = true,
        .cap_full_refresh = true,
        .cap_waveform_select = true,
    },
    {
        /* 占位：常见的 296x152 SPI 直连控制器（横向连续布局）。
         * 未在本项目验证过——保留它是为了让「换 SoC 只改一处」这件事
         * 有具体落点：填对描述符即可，不必改 raster/policy/display。 */
        .name = "generic-296x152-linear",
        .hor_res = 296,
        .ver_res = 152,
        .strip_h = QZOS_STRIP_H,
        .layout = QZOS_FRAME_LINEAR,
        .dev_path = "/dev/epaper_lcd",
        .refresh_path = NULL,
        .waveform_path = NULL,
        .dev_writable_frame = true,
        .frame_bytes = 0,
        .cap_dirty = true,
        .cap_full_refresh = false, /* 无 refresh 机制，只能靠驱动内部波形 */
        .cap_waveform_select = false,
    },
};

#define N_BUILTIN ((int)(sizeof(s_builtin) / sizeof(s_builtin[0])))

/* ---- 注册表查询 ---- */

const qzos_panel_t *qzos_panel_builtin(int *idx)
{
    int i = idx ? *idx : 0;
    if (i < 0 || i >= N_BUILTIN) return NULL;
    if (idx) *idx = i + 1;
    return &s_builtin[i];
}

const qzos_panel_t *qzos_panel_by_name(const char *name)
{
    if (!name || !*name) return NULL;
    for (int i = 0; i < N_BUILTIN; i++) {
        if (strcmp(s_builtin[i].name, name) == 0) return &s_builtin[i];
    }
    return NULL;
}

const qzos_panel_t *qzos_panel_default(void)
{
    const char *env = getenv("QZ_PANEL");
    const qzos_panel_t *p = env && *env ? qzos_panel_by_name(env) : NULL;
    if (p) return p;
    if (env && *env) {
        fprintf(stderr, "qzos-panel: unknown QZ_PANEL='%s', using mp-d261\n", env);
    }
    return &s_builtin[0];
}

uint32_t qzos_panel_frame_bytes(const qzos_panel_t *p)
{
    if (!p) return 0;
    if (p->frame_bytes) return p->frame_bytes;
    if (p->hor_res <= 0 || p->ver_res <= 0) return 0;

    switch (p->layout) {
    case QZOS_FRAME_LINEAR:
        return (uint32_t)(p->hor_res / 8) * (uint32_t)p->ver_res;
    case QZOS_FRAME_STRIP8:
    default:
        if (p->strip_h <= 0) return 0;
        return (uint32_t)p->hor_res * (uint32_t)(p->ver_res / p->strip_h);
    }
}

/* ---- 运行期覆盖（可移植性：换 SoC 时的主要手段） ---- */

/* 环境变量覆盖后的生效描述符。QZ_PANEL_DEV / QZ_PANEL_REFRESH /
 * QZ_PANEL_WAVEFORM 允许在不改代码的前提下指向另一块屏。
 * 只在首次访问时解析一次，之后缓存。 */
static qzos_panel_t s_active;
static bool s_active_init;
static int s_fd = -1;

const qzos_panel_t *qzos_panel_active(void)
{
    if (!s_active_init) {
        const qzos_panel_t *base = qzos_panel_default();
        s_active = *base;
        const char *e;
        if ((e = getenv("QZ_PANEL_DEV")) != NULL && *e) {
            s_active.dev_path = e;
            s_active.dev_writable_frame = true;
        }
        if ((e = getenv("QZ_PANEL_REFRESH")) != NULL) {
            s_active.refresh_path = *e ? e : NULL;
            s_active.cap_full_refresh = s_active.refresh_path != NULL;
        }
        if ((e = getenv("QZ_PANEL_WAVEFORM")) != NULL) {
            s_active.waveform_path = *e ? e : NULL;
            s_active.cap_waveform_select = s_active.waveform_path != NULL;
        }
        s_active_init = true;
    }
    return &s_active;
}

/* ---- 设备 I/O ---- */

static int write_sysfs(const char *path, const char *val)
{
    if (!path || !*path) return -1;
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return (n == (ssize_t)strlen(val)) ? 0 : -1;
}

int qzos_panel_open(const qzos_panel_t *p)
{
    if (!p) return -1;
    if (!p->dev_path || !*p->dev_path) return -1;
    if (!p->dev_writable_frame) return -1;
    if (s_fd >= 0) return s_fd;

    s_fd = open(p->dev_path, O_WRONLY | O_NONBLOCK);
    return s_fd;
}

void qzos_panel_close(void)
{
    if (s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
}

bool qzos_panel_available(void)
{
    return s_fd >= 0;
}

int qzos_panel_present(const qzos_panel_t *p, const uint8_t *frame,
                       uint32_t len, qzos_waveform_t wf)
{
    if (!p || !frame) return -1;

    uint32_t expect = qzos_panel_frame_bytes(p);
    if (expect == 0 || len < expect) {
        fprintf(stderr, "qzos-panel: bad frame len %u (expect %u)\n", len, expect);
        return -1;
    }

    /* 波形选择：FAST 打开「仅快速刷新」，FULL 关闭（允许全刷波形）。
     * 面板没有该机制时静默跳过——驱动内部仍会按帧差分选波形。 */
    if (p->cap_waveform_select && p->waveform_path) {
        (void)write_sysfs(p->waveform_path, (wf == QZOS_WF_FAST) ? "1" : "0");
    }

    /* 帧提交：驱动只接受整帧 */
    if (!p->dev_writable_frame || !p->dev_path) return 0; /* 诊断模式：出图不落屏 */

    int fd = qzos_panel_open(p);
    if (fd < 0) return -1;

    ssize_t n = write(fd, frame, expect);
    if (n != (ssize_t)expect) {
        fprintf(stderr, "qzos-panel: short write %d/%u\n", (int)n, expect);
        /* 写失败后 fd 状态不可信，下次重开 */
        qzos_panel_close();
        return -1;
    }

    /* 全刷：清 e-ink 残影。写在帧之后（与 C1Terminal 行为一致）。 */
    if (wf == QZOS_WF_FULL && p->cap_full_refresh) {
        if (write_sysfs(p->refresh_path, "1") != 0) {
            fprintf(stderr, "qzos-panel: full refresh trigger failed (%s)\n",
                    p->refresh_path ? p->refresh_path : "(none)");
            return -1;
        }
    }
    return 0;
}
