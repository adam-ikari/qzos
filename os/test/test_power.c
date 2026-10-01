/* test_power.c — 电源域决策层单测（纯逻辑，无 I/O、无设备）
 *
 * 覆盖的是 os/docs/power-sim.md §3 的场景矩阵，尤其是**场景 D**：
 * key_owner 未知时必须拒绝一切写动作。
 *
 * 这条是本层唯一不可协商的不变量，挡的是「双重处理」——若内核或厂商 pmd 已经
 * 在收电源键而 qzos 也去 suspend 一次，症状是设备在两层逻辑间来回跳，
 * 在真机上极难归因，因为两边都「看起来在工作」。
 */
#include "power.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks, failed;
static char failures[64][192];
static int nfail;

static void ok(int cond, const char *name)
{
    checks++;
    if (cond) return;
    /* failed 必须在这里 ++。
     * 写这一行时漏了它：ok() 只把名字记进 failures[] 数组，failed 一直是 0，
     * 于是结尾的 `if (failed == 0) printf("OK: ...")` 永远走真——闸门变成了
     * 装饰。发现方式是变异测试：把「未知归属全拒」那条不变量从 power.c 里
     * 删掉，43 条断言**照样全绿**。
     * 与 brain 里那条「回执层面的断言不能替代效果层面的断言」同一个病：
     * 判据要能发现自己坏了。 */
    failed++;
    if (nfail < 64) snprintf(failures[nfail++], 192, "%s", name);
}

static const char *why;
static int may(const qzos_power_t *p, qzos_power_action_t a)
{
    why = NULL;
    return qzos_power_may_act(p, a, &why);
}

static qzos_power_t mk(qzos_key_owner_t owner, bool susp, bool shut, bool reb)
{
    qzos_power_t p;
    memset(&p, 0, sizeof(p));
    p.name = "test";
    p.key_owner = owner;
    p.cap_suspend = susp; p.suspend_path  = susp  ? "/x/s" : NULL;
    p.cap_shutdown = shut; p.shutdown_path = shut ? "/x/o" : NULL;
    p.cap_reboot = reb;    p.reboot_path    = reb   ? "/x/r" : NULL;
    return p;
}

int main(void)
{
    /* ---- 场景 D：归属未知 → 三个动作全拒，且必须给出原因 ---- */
    {
        qzos_power_t p = mk(QZOS_KEY_OWNER_UNKNOWN, 1, 1, 1); /* 能力全开 */
        ok(!may(&p, QZOS_PWR_SUSPEND),  "D: 未知归属拒 suspend（即便能力位为真）");
        ok(!may(&p, QZOS_PWR_SHUTDOWN), "D: 未知归属拒 shutdown（即便能力位为真）");
        ok(!may(&p, QZOS_PWR_REBOOT),   "D: 未知归属拒 reboot（即便能力位为真）");
        ok(why && strstr(why, "双重处理") != NULL,
           "D: 拒绝理由要说清是防双重处理（要能直接显示给用户）");
    }

    /* ---- 场景 A：内核管 → 全部不重复触发 ---- */
    {
        qzos_power_t p = mk(QZOS_KEY_OWNER_KERNEL, 1, 1, 1);
        ok(!may(&p, QZOS_PWR_SUSPEND),  "A: 内核管则不重复 suspend");
        ok(!may(&p, QZOS_PWR_SHUTDOWN), "A: 内核管则不重复 shutdown");
        ok(!may(&p, QZOS_PWR_REBOOT),   "A: 内核管则不重复 reboot");
        ok(why && strstr(why, "内核") != NULL, "A: 理由提到内核");
    }

    /* ---- 场景 B：厂商守护进程管 → 转发而非绕过 ---- */
    {
        qzos_power_t p = mk(QZOS_KEY_OWNER_VENDOR, 1, 1, 1);
        ok(!may(&p, QZOS_PWR_SUSPEND),  "B: 厂商管则不绕过");
        ok(!may(&p, QZOS_PWR_SHUTDOWN), "B: 厂商管则不绕过 shutdown");
        ok(why && strstr(why, "转发") != NULL,
           "B: 理由说的是「转发」而不是「不能做」——将来要接转发路径");
    }

    /* ---- 场景 C：没人管 → 按能力位放行 ---- */
    {
        qzos_power_t p = mk(QZOS_KEY_OWNER_NONE, 1, 1, 1);
        ok(may(&p, QZOS_PWR_SUSPEND),  "C: 无人管 + 有 suspend → 放行");
        ok(may(&p, QZOS_PWR_SHUTDOWN), "C: 无人管 + 有 shutdown → 放行");
        ok(may(&p, QZOS_PWR_REBOOT),   "C: 无人管 + 有 reboot → 放行");
    }

    /* ---- 场景 D+：我们就是系统 → 同样按能力位 ---- */
    {
        qzos_power_t p = mk(QZOS_KEY_OWNER_QZOS, 1, 0, 0);
        ok(may(&p, QZOS_PWR_SUSPEND),  "qzos: 有 suspend → 放行");
        ok(!may(&p, QZOS_PWR_SHUTDOWN), "qzos: 无 shutdown 路径 → 拒（路径缺失优先于归属）");
    }

    /* ---- 能力位与路径必须成对 ----
     * 光有 cap 没路径、或光有路径没 cap，都不该放行。前者是描述符写错，
     * 后者会让「未验就开能力」这种事漏过去。 */
    {
        qzos_power_t p = mk(QZOS_KEY_OWNER_NONE, 1, 1, 1);
        p.cap_shutdown = false;                 /* cap 关、路径在 */
        ok(!may(&p, QZOS_PWR_SHUTDOWN), "cap 关时即便路径存在也拒（不能靠路径绕过能力位）");
        p.cap_shutdown = true;
        p.shutdown_path = NULL;                 /* cap 开、路径无 */
        ok(!may(&p, QZOS_PWR_SHUTDOWN), "路径缺失时拒（cap 位不能凭空放行）");
    }

    /* ---- 关机是最高危动作：默认描述符必须拒 ----
     * 若设备没有可写的 poweroff sysfs，「关机」只能靠 sysrq 或直接断电，
     * 可能损坏文件系统——而这台设备熄屏后只能靠 USB ADB 救。 */
    {
        const qzos_power_t *d = qzos_power_default();
        ok(d != NULL, "有缺省描述符");
        if (d) {
            ok(d->key_owner == QZOS_KEY_OWNER_UNKNOWN, "缺省归属是 unknown（不是猜一个）");
            ok(!may(d, QZOS_PWR_SHUTDOWN), "缺省拒绝 shutdown");
            ok(!may(d, QZOS_PWR_SUSPEND),  "缺省拒绝 suspend");
            ok(d->shutdown_path == NULL,   "缺省没有 shutdown 路径");
        }
        const qzos_power_t *u = qzos_power_by_name("mp-d261-unverified");
        ok(u != NULL, "有目标设备描述符（标注未验）");
        if (u) {
            /* 未验后端必须把高危能力关掉，让误用被 may_act 挡住，
             * 直到有人真机验证后显式改过来。 */
            ok(!u->cap_shutdown, "未验后端 cap_shutdown 为假（P4 未验）");
            ok(!u->cap_suspend,  "未验后端 cap_suspend 为假（P3 未验）");
            ok(!may(u, QZOS_PWR_SHUTDOWN), "未验后端拒 shutdown");
            ok(u->key_owner == QZOS_KEY_OWNER_UNKNOWN, "未验后端归属 unknown（P2 未验）");
        }
    }

    /* ---- 注册表 ---- */
    {
        ok(qzos_power_by_name("none") != NULL, "按名字取 none");
        ok(qzos_power_by_name("sim-fake") != NULL, "按名字取 sim-fake");
        ok(qzos_power_by_name("no-such-thing") == NULL, "未知名字返回 NULL（不回退到猜的）");
        ok(qzos_power_by_name(NULL) == NULL, "NULL 名字返回 NULL");
        int n = 0;
        while (qzos_power_builtin(&n)) { }
        ok(n >= 3, "内置描述符至少 3 个");
    }

    /* ---- 名称映射（UI 与日志都要用，错字会出现在屏上）---- */
    {
        ok(strcmp(qzos_power_owner_name(QZOS_KEY_OWNER_UNKNOWN), "unknown") == 0, "owner unknown");
        ok(strcmp(qzos_power_owner_name(QZOS_KEY_OWNER_KERNEL),  "kernel")  == 0, "owner kernel");
        ok(strcmp(qzos_power_owner_name(QZOS_KEY_OWNER_VENDOR),  "vendor")  == 0, "owner vendor");
        ok(strcmp(qzos_power_owner_name(QZOS_KEY_OWNER_NONE),    "none")    == 0, "owner none");
        ok(strcmp(qzos_power_owner_name(QZOS_KEY_OWNER_QZOS),    "qzos")    == 0, "owner qzos");
        ok(strcmp(qzos_power_action_name(QZOS_PWR_SUSPEND),  "suspend")  == 0, "act suspend");
        ok(strcmp(qzos_power_action_name(QZOS_PWR_SHUTDOWN), "shutdown") == 0, "act shutdown");
        ok(strcmp(qzos_power_action_name(QZOS_PWR_REBOOT),   "reboot")   == 0, "act reboot");
    }

    /* ---- NULL 描述符 ---- */
    ok(!may(NULL, QZOS_PWR_SUSPEND), "NULL 描述符拒动作");
    ok(why && strstr(why, "描述符") != NULL, "NULL 描述符有可显示的理由");

    /* ---- qzos_power_active()：QZ_POWER_* 覆盖 + 「给了路径就开 cap」的耦合 ----
     *
     * 这个函数此前**一条测试都没有**，而它恰恰是决定真机能力的那一个：
     * `if (s_active.shutdown_path) s_active.cap_shutdown = true;` 意思是
     * 「显式配置即显式承诺」。于是**填一个路径就等于开一个能力**——而闸门 0
     * 的 P3/P4 正是「该填哪些路径」这件事本身还没查清。
     *
     * 用 fork 而不是给生产代码加 reset：qzos_power_active() 把结果缓存在
     * 文件作用域的 s_active_ready，同一进程内只能测一个场景。fork 出的子进程
     * 拿到的是未初始化过的副本，语义上就是「一个刚启动的宿主」。
     */
    {
        static const struct {
            const char *name;
            const char *backend;
            const char *owner;      /* QZ_POWER_KEY_OWNER */
            const char *shutdown;   /* QZ_POWER_SHUTDOWN */
            int         may_shutdown;
            const char *expect;
        } sc[] = {
            /* 缺省：什么都不做 */
            { "default-none",        NULL,  NULL,  NULL,           0, "none" },
            /* 未知后端名 → 退回 none，而不是猜一个 */
            { "unknown-backend",     "no-such-thing", NULL, NULL,  0, "none" },
            /* sim-fake 归属 none，但路径全空 → 仍拒（路径才是 load-bearing） */
            { "sim-no-path",         "sim-fake", NULL, NULL,       0, "sim-fake" },
            /* sim-fake + 只填 shutdown 路径 → cap 自动开 → 允许 */
            { "sim-with-path",       "sim-fake", NULL, "/x/shut",  1, "sim-fake" },
            /* 认不出的归属串 → unknown（fail-closed），不是「保持原值」 */
            { "garbage-owner",       "sim-fake", "wat", "/x/shut", 0, "sim-fake" },
            /* **关键一条**：真机描述符 + 填了 shutdown 路径，但归属仍 UNKNOWN
             * → 仍然拒。这证明「顺手把 P4 路径填上」不会顺带打开关机。 */
            { "device-path-only",    "mp-d261-unverified", NULL, "/x/shut", 0, "mp-d261-unverified" },
            /* 归属确认成 none 之后，同一个路径才真的生效 */
            { "device-owner-none",   "mp-d261-unverified", "none", "/x/shut", 1, "mp-d261-unverified" },
        };
        for (unsigned i = 0; i < sizeof(sc) / sizeof(sc[0]); i++) {
            pid_t pid = fork();
            if (pid == 0) {
                /* 子进程：设 env → 取 active → 断言 → 用退出码回报。
                 * 断言结果不能走父进程的计数器（那是 fork 前的副本）。 */
                checks = failed = nfail = 0;
                if (sc[i].backend) setenv("QZ_POWER", sc[i].backend, 1);
                if (sc[i].owner)   setenv("QZ_POWER_KEY_OWNER", sc[i].owner, 1);
                if (sc[i].shutdown) setenv("QZ_POWER_SHUTDOWN", sc[i].shutdown, 1);
                const qzos_power_t *a = qzos_power_active();
                int bad = 0, got = -1;
                if (a) {
                    got = may(a, QZOS_PWR_SHUTDOWN) ? 1 : 0;
                    if (got != sc[i].may_shutdown) bad = 1;
                    if (strcmp(a->name, sc[i].expect) != 0) bad = 1;
                } else {
                    bad = 1;
                }
                if (!bad) printf("  ok   %s\n", sc[i].name);
                else       printf("  BAD  %s (backend=%s owner=%s may_shutdown=%d 期望=%d)\n",
                                  sc[i].name, a ? a->name : "(null)",
                                  a ? qzos_power_owner_name(a->key_owner) : "-",
                                  got, sc[i].may_shutdown);
                fflush(stdout);
                _exit(bad ? 1 : 0);
            }
            int st = 0;
            waitpid(pid, &st, 0);
            ok(WIFEXITED(st) && WEXITSTATUS(st) == 0, sc[i].name);
        }
    }

    if (failed == 0) {
        printf("OK: %d checks, 0 failed\n", checks);
    } else {
        printf("FAILED: %d of %d\n", failed, checks);
        for (int i = 0; i < nfail; i++) printf("  - %s\n", failures[i]);
    }
    return failed ? 1 : 0;
}
