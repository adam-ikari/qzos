/*
 * power.h — 电源域描述符与决策层（C99）
 *
 * 与 panel.h 同构：「换设备只改一处」的描述符模式。这是本仓已验证的可移植性
 * 写法（见 os/src/panel.c 的 s_builtin[]）。
 *
 * 分层纪律（brain: qzos-js-first / qzos-power-sim）：
 *   这一层只做「我能不能做」+ I/O；**不做**策略决策（什么时候提示关机、
 *   省电模式怎么选）——那些是 JS 侧的事。
 *
 * 最重要的一条规矩：**key_owner 未知时拒绝一切写动作。** 在没查清的设备上
 * 唯一安全的动作是「不动」。一个「大概是内核在管所以我也顺手处理一下」的
 * 实现，症状是双重 suspend 或与厂商 pmd 打架，而那在真机上极难归因。
 */
#ifndef QZOS_POWER_H
#define QZOS_POWER_H

#include <stdbool.h>
#include <stdint.h>

/* ---- 电源键的既有归属：闸门 0 的核心未知 ---- */
typedef enum {
    QZOS_KEY_OWNER_UNKNOWN = 0, /* 查不出来 → 拒绝一切写动作 */
    QZOS_KEY_OWNER_KERNEL  = 1, /* 内核自己收（场景 A） */
    QZOS_KEY_OWNER_VENDOR  = 2, /* 厂商守护进程收（场景 B） */
    QZOS_KEY_OWNER_NONE    = 3, /* 没人管，我们可以接管（场景 C） */
    QZOS_KEY_OWNER_QZOS    = 4  /* 我们自己就是系统（场景 D 之后的状态） */
} qzos_key_owner_t;

typedef struct {
    const char *name;

    /* ---- 电源键 ---- */
    const char *key_dev;        /* 承载电源键的 evdev；NULL = 不从这里收 */
    int         key_code;       /* KEY_POWER / KEY_SLEEP，按设备实测填 */
    qzos_key_owner_t key_owner;
    const char *key_owner_detail; /* vendor 时填进程路径，仅供显示与日志 */

    /* ---- 状态读取（路径不存在时读不到，不许编默认值）---- */
    const char *state_path;     /* power_supply/BAT0/status */
    const char *capacity_path;  /* power_supply/BAT0/capacity */
    const char *charge_path;    /* power_supply/BAT0/status 里的 Charging 判定 */

    /* ---- 动作（NULL = 该设备不支持）---- */
    const char *suspend_path;   /* 写 "mem" */
    const char *shutdown_path;  /* 写 "1" */
    const char *reboot_path;    /* 写 "1" */

    bool cap_battery;
    bool cap_suspend;
    bool cap_shutdown;
    bool cap_reboot;
} qzos_power_t;

typedef enum {
    QZOS_PWR_SUSPEND = 0,
    QZOS_PWR_SHUTDOWN = 1,
    QZOS_PWR_REBOOT = 2
} qzos_power_action_t;

typedef struct {
    bool     present;      /* 读到了状态（false = 别把 0 当成「没电」）*/
    int      percent;      /* -1 = 未知 */
    bool     charging;
    qzos_key_owner_t owner;
} qzos_power_state_t;

/* ---- 注册表（形状照 panel.h）---- */
const qzos_power_t *qzos_power_by_name(const char *name);
const qzos_power_t *qzos_power_default(void);
const qzos_power_t *qzos_power_active(void);   /* + QZ_POWER_* 环境覆盖 */
const qzos_power_t *qzos_power_builtin(int *idx);

const char *qzos_power_owner_name(qzos_key_owner_t o);
const char *qzos_power_action_name(qzos_power_action_t a);

/* ---- 决策：这才是本层真正的价值所在 ----
 *
 * 纯函数，无 I/O，可单测。回答「qzos 现在该不该执行这个动作」。
 * 返回 false 时 *why（若非 NULL）拿到一句能直接显示给用户的中文原因。 */
bool qzos_power_may_act(const qzos_power_t *p, qzos_power_action_t act,
                        const char **why);

/* ---- I/O（唯一碰设备的地方）---- */
bool qzos_power_read(const qzos_power_t *p, qzos_power_state_t *out);
int  qzos_power_request(const qzos_power_t *p, qzos_power_action_t act);

/* 目标设备路径（供 shell 显示与诊断；未配置时返回 NULL）*/
const char *qzos_power_owner_detail(const qzos_power_t *p);

#endif /* QZOS_POWER_H */
