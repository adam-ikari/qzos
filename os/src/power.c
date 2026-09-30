/*
 * power.c — 电源域描述符与决策层
 *
 * 设计稿 os/docs/power-sim.md；决策记录 brain qzos-power-sim。
 *
 * 这一层分两部分，可测性差别很大，所以分开写：
 *
 *   1. 决策（qzos_power_may_act）—— 纯函数、无 I/O、设备无关。
 *      **这是安全关键的部分，也是唯一能在没有真机时被真正验证的部分。**
 *
 *   2. I/O（qzos_power_read / _request）—— 碰 sysfs 路径。
 *      路径与语义全部未在真机验证过（brain qzos-power-sim 的 P1–P7），
 *      所以这里刻意写得很薄：读不到就报「读不到」，动作做不了就报错，
 *      任何地方都不猜、不填默认值。
 */
#include "power.h"

#include <stddef.h>   /* offsetof */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---- 内置描述符 ----
 *
 * 三个后端刻意对应「知道多少」的三档，而不是「设备型号」的三档：
 *   none         —— 什么都不做（未知设备的缺省；拒绝写动作）
 *   sim-fake     —— 路径全在环境变量里，指向假文件树，供场景矩阵测试
 *   mp-d261-*    —— 目标设备的**猜测**形状，未验证（P1–P7 全未验）
 */

static const qzos_power_t s_builtin[] = {
    {
        /* 缺省：什么都不做。
         * 选它做缺省而不是猜一个，是因为「不知道」时唯一安全的动作是
         * 「不动」——见文件头的 key_owner 规矩。 */
        .name = "none",
        .key_dev = NULL, .key_code = 0,
        .key_owner = QZOS_KEY_OWNER_UNKNOWN, .key_owner_detail = NULL,
        .state_path = NULL, .capacity_path = NULL, .charge_path = NULL,
        .suspend_path = NULL, .shutdown_path = NULL, .reboot_path = NULL,
        .cap_battery = false, .cap_suspend = false,
        .cap_shutdown = false, .cap_reboot = false,
    },
    {
        /* 仿真后端：所有路径都留空，由 qzos_power_active() 用 QZ_POWER_*
         * 环境变量填。好处是**不为仿真发明任何新机制**——qzos 访问设备本来
         * 就是通过路径，把路径指向临时目录里的假文件就是仿真。 */
        .name = "sim-fake",
        .key_dev = NULL, .key_code = 116 /* KEY_POWER */,
        .key_owner = QZOS_KEY_OWNER_NONE, /* 场景 C 默认；QZ_POWER_OWNER 可改 */
        .key_owner_detail = "sim",
        .state_path = NULL, .capacity_path = NULL, .charge_path = NULL,
        .suspend_path = NULL, .shutdown_path = NULL, .reboot_path = NULL,
        .cap_battery = true, .cap_suspend = true,
        .cap_shutdown = true, .cap_reboot = true,
    },
    {
        /* 目标设备（C1 Slim / MP-D261）的**猜测**形状。
         *
         * ⚠ 以下每一条都是猜的，且**没有一条在真机上验证过**：
         *   - 电源键在哪个 evdev、键码是 KEY_POWER 还是别的（P1）
         *   - 是否存在可写的 suspend/poweroff sysfs（P3/P4）—— 这条最危险：
         *     若不存在，「关机」就只能靠 sysrq 或直接断电，等于可能损坏
         *     文件系统。所以下面把 cap_shutdown/cap_reboot 都设成 false，
         *     让任何误用都被 qzos_power_may_act 挡住，直到有人真机验证后
         *     显式改过来。
         *   - 电池节点名（BAT0？BAT1？根本没有？）—— P6
         * 拿到真机后请用 scripts/probe-power-owner.sh 的输出替换本表，
         * 逐条对应台账里的 P 编号。 */
        .name = "mp-d261-unverified",
        .key_dev = NULL, .key_code = 116,
        .key_owner = QZOS_KEY_OWNER_UNKNOWN,   /* P2 未验 → 拒绝写动作 */
        .key_owner_detail = NULL,
        .state_path = "/sys/class/power_supply/BAT0/status",
        .capacity_path = "/sys/class/power_supply/BAT0/capacity",
        .charge_path = "/sys/class/power_supply/BAT0/status",
        .suspend_path = NULL,   /* P3 未验 → 不填 */
        .shutdown_path = NULL,  /* P4 未验 → 不填 */
        .reboot_path = NULL,
        .cap_battery = true,
        .cap_suspend = false,  /* 未验前一律 false，让 may_act 挡住 */
        .cap_shutdown = false,
        .cap_reboot = false,
    },
};

#define N_BUILTIN ((int)(sizeof(s_builtin) / sizeof(s_builtin[0])))

/* ---- 注册表查询 ---- */

const qzos_power_t *qzos_power_builtin(int *idx)
{
    if (!idx) return NULL;
    if (*idx < 0 || *idx >= N_BUILTIN) return NULL;
    return &s_builtin[(*idx)++];
}

const qzos_power_t *qzos_power_by_name(const char *name)
{
    if (!name || !*name) return NULL;
    for (int i = 0; i < N_BUILTIN; i++) {
        if (strcmp(s_builtin[i].name, name) == 0) return &s_builtin[i];
    }
    return NULL;
}

const qzos_power_t *qzos_power_default(void)
{
    /* 缺省是 none（= 不动），不是 mp-d261-unverified。理由：后者的
     * key_owner 是 UNKNOWN，两者在决策上等价，但 none 连状态路径都不碰，
     * 不会在没插设备时就去读一堆不存在的 sysfs。 */
    return &s_builtin[0];
}

/* 生效描述符 = 选中的后端 + QZ_POWER_* 环境覆盖。
 * 覆盖项逐个判空，所以 sim-fake 可以只给需要的路径。 */
static qzos_power_t s_active;
static bool s_active_ready;

const qzos_power_t *qzos_power_active(void)
{
    if (s_active_ready) return &s_active;

    const char *sel = getenv("QZ_POWER");
    const qzos_power_t *base = qzos_power_by_name(sel);
    if (!base) {
        if (sel && *sel)
            fprintf(stderr, "qzos-power: unknown backend '%s', falling back to none\n", sel);
        base = qzos_power_default();
    }
    s_active = *base;

    struct { const char *env; size_t off; } strs[] = {
        { "QZ_POWER_STATE",    offsetof(qzos_power_t, state_path) },
        { "QZ_POWER_CAPACITY", offsetof(qzos_power_t, capacity_path) },
        { "QZ_POWER_SUSPEND",  offsetof(qzos_power_t, suspend_path) },
        { "QZ_POWER_SHUTDOWN", offsetof(qzos_power_t, shutdown_path) },
        { "QZ_POWER_REBOOT",   offsetof(qzos_power_t, reboot_path) },
        { "QZ_POWER_OWNER",    offsetof(qzos_power_t, key_owner_detail) },
    };
    for (unsigned i = 0; i < sizeof(strs) / sizeof(strs[0]); i++) {
        const char *v = getenv(strs[i].env);
        if (v && *v) *(const char **)((char *)&s_active + strs[i].off) = v;
    }
    const char *kd = getenv("QZ_POWER_KEY_DEV");
    if (kd && *kd) s_active.key_dev = kd;

    /* 归属可由环境改（probe-power-owner.sh 的输出就是喂这个）。
     * 认 "kernel"/"vendor"/"none"/"qzos"/"unknown"；认不出来就当 unknown，
     * 也就是「拒绝动作」——宁可不动。 */
    const char *own = getenv("QZ_POWER_KEY_OWNER");
    if (own && *own) {
        if      (strcmp(own, "kernel") == 0) s_active.key_owner = QZOS_KEY_OWNER_KERNEL;
        else if (strcmp(own, "vendor") == 0) s_active.key_owner = QZOS_KEY_OWNER_VENDOR;
        else if (strcmp(own, "none")   == 0) s_active.key_owner = QZOS_KEY_OWNER_NONE;
        else if (strcmp(own, "qzos")   == 0) s_active.key_owner = QZOS_KEY_OWNER_QZOS;
        else s_active.key_owner = QZOS_KEY_OWNER_UNKNOWN;
    }
    /* 给了 suspend/shutdown 路径就认为该动作被支持——显式配置即显式承诺。
     * 反过来不自动开 cap：mp-d261 那个未验后端正因为 cap=false 而安全。 */
    if (s_active.suspend_path)  s_active.cap_suspend = true;
    if (s_active.shutdown_path) s_active.cap_shutdown = true;
    if (s_active.reboot_path)    s_active.cap_reboot = true;

    s_active_ready = true;
    return &s_active;
}

const char *qzos_power_owner_name(qzos_key_owner_t o)
{
    switch (o) {
    case QZOS_KEY_OWNER_KERNEL: return "kernel";
    case QZOS_KEY_OWNER_VENDOR: return "vendor";
    case QZOS_KEY_OWNER_NONE:   return "none";
    case QZOS_KEY_OWNER_QZOS:   return "qzos";
    default:                    return "unknown";
    }
}

const char *qzos_power_action_name(qzos_power_action_t a)
{
    switch (a) {
    case QZOS_PWR_SUSPEND:  return "suspend";
    case QZOS_PWR_SHUTDOWN: return "shutdown";
    case QZOS_PWR_REBOOT:   return "reboot";
    default:                return "?";
    }
}

const char *qzos_power_owner_detail(const qzos_power_t *p)
{
    return p ? p->key_owner_detail : NULL;
}

/* ---- 决策：安全关键的部分 ----
 *
 * 场景矩阵见 os/docs/power-sim.md §3。核心不变量：
 *
 *   **key_owner == UNKNOWN ⇒ 一切写动作被拒。**
 *
 * 这条不变量挡的是「双重处理」：若内核或厂商 pmd 已经在收电源键，而 qzos
 * 也去 suspend 一次，症状是设备在两层逻辑间来回跳；这种问题在真机上极难
 * 归因，因为两边都「看起来在工作」。
 */

bool qzos_power_may_act(const qzos_power_t *p, qzos_power_action_t act,
                        const char **why)
{
    if (why) *why = NULL;
    if (!p) { if (why) *why = "没有电源描述符"; return false; }

    /* 1. 归属未知 → 全拒。这是本层唯一一条不可协商的规则。 */
    if (p->key_owner == QZOS_KEY_OWNER_UNKNOWN) {
        if (why) *why = "电源键归属未确认，拒绝执行（防双重处理）";
        return false;
    }
    /* 2. 归内核管 → 只有 suspend 是内核自己的事，qzos 不重复触发；
     *    关机/重启同理，交给内核或厂商。 */
    if (p->key_owner == QZOS_KEY_OWNER_KERNEL) {
        if (why) *why = "电源由内核管理，qzos 不重复触发";
        return false;
    }
    /* 3. 归厂商守护进程管 → 转发而不是绕过（绕过会与它打架）。 */
    if (p->key_owner == QZOS_KEY_OWNER_VENDOR) {
        if (why) *why = "由厂商守护进程管理，应转发而非绕过";
        return false;
    }
    /* 4. 到这里只剩 NONE（没人管）与 QZOS（我们就是系统）。 */
    switch (act) {
    case QZOS_PWR_SUSPEND:
        if (!p->cap_suspend || !p->suspend_path) {
            if (why) *why = "该设备不支持 suspend（路径未配置或未验证）";
            return false;
        }
        break;
    case QZOS_PWR_SHUTDOWN:
        if (!p->cap_shutdown || !p->shutdown_path) {
            /* 这条最可能挡住真机：P3/P4 未验时故意 false。
             * 理由写在描述符里：若设备没有可写的 poweroff sysfs，「关机」
             * 只能靠 sysrq 或直接断电，可能损坏文件系统——宁可不做。 */
            if (why) *why = "该设备的关机路径未验证，拒绝（防断电损坏文件系统）";
            return false;
        }
        break;
    case QZOS_PWR_REBOOT:
        if (!p->cap_reboot || !p->reboot_path) {
            if (why) *why = "该设备不支持 reboot（路径未配置或未验证）";
            return false;
        }
        break;
    default:
        if (why) *why = "未知动作";
        return false;
    }
    return true;
}

/* ---- I/O ----
 *
 * 全部未在真机验证（P1–P7）。所以规则是：读不到就说读不到，做不了就报错。
 * 任何地方都不猜、不填默认值——「一个假的 50% 电量」比「不知道」危险得多。
 */

static bool read_line(const char *path, char *buf, size_t n)
{
    if (!path || !*path) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    if (!fgets(buf, (int)n, f)) { fclose(f); return false; }
    fclose(f);
    size_t l = strlen(buf);
    while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = '\0';
    return l > 0;
}

bool qzos_power_read(const qzos_power_t *p, qzos_power_state_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->percent = -1;                 /* -1 = 未知，绝不用 0 冒充 */
    out->owner = p ? p->key_owner : QZOS_KEY_OWNER_UNKNOWN;
    if (!p) return false;

    char buf[64];
    bool any = false;

    if (p->state_path && read_line(p->state_path, buf, sizeof(buf))) {
        out->present = true;
        any = true;
        /* power_supply 的 status 是 Charging / Discharging / Full 等 */
        if (strstr(buf, "Charging")) out->charging = true;
    }
    if (p->capacity_path && read_line(p->capacity_path, buf, sizeof(buf))) {
        char *end = NULL;
        long v = strtol(buf, &end, 10);
        /* 只接受合理区间：设备给 -1（未知）或 101+（估算超出）时报未知，
         * 不用一个越界数字去驱动「低电量」提示。 */
        if (end && v >= 0 && v <= 100) { out->percent = (int)v; any = true; }
        else out->percent = -1;
    }
    (void)any;
    return out->present;
}

int qzos_power_request(const qzos_power_t *p, qzos_power_action_t act)
{
    const char *why = NULL;
    if (!qzos_power_may_act(p, act, &why)) {
        fprintf(stderr, "qzos-power: refuse %s: %s\n",
                qzos_power_action_name(act), why ? why : "(no reason)");
        return -1;
    }
    const char *path = (act == QZOS_PWR_SUSPEND)  ? p->suspend_path
                     : (act == QZOS_PWR_SHUTDOWN) ? p->shutdown_path
                                                  : p->reboot_path;
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "qzos-power: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    const char *val = (act == QZOS_PWR_SUSPEND) ? "mem" : "1";
    int rc = (fputs(val, f) < 0) ? -1 : 0;
    if (fclose(f) != 0) rc = -1;
    fprintf(stderr, "qzos-power: %s -> wrote '%s' to %s (rc=%d)\n",
            qzos_power_action_name(act), val, path, rc);
    return rc;
}
