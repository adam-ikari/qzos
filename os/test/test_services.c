/* test_services.c — 系统服务面注册表单测（纯逻辑，无 I/O）
 *
 * 收口完成的判据不是「代码看起来分了层」，而是**注册表之外无 C 能力**。
 * 早期授权检查挂在渲染桥的 op_rpc 上，那只挡住了那一条通道——而 JS 还可以
 * 走别的路碰到 C。挂错位置的检查等于没有检查。
 *
 * 这里把注册表暴露成可查询的，然后逐条核对三件事：
 *   1. 授权检查在服务面侧（不在渲染桥）
 *   2. 缺省全拒（没设授权就调 → 拒）
 *   3. 带能力的方法必须落在自己能力的命名空间里（cap 不是谎言）
 *
 * 还需要 host 才能验的部分（从 JS 穷举可达路径）由
 * scripts/verify-mips-e2e.sh 与 os/test/test_shell_apps.sh 覆盖。
 */
#include "qzos.h"

#include <stdio.h>
#include <string.h>

static int checks, failed;
static char failures[64][192];
static int nfail;

static void ok(int cond, const char *name)
{
    checks++;
    if (cond) return;
    failed++;
    if (nfail < 64) snprintf(failures[nfail++], 192, "%s", name);
}

static void set_caps(const char *id, const char *a, const char *b)
{
    char caps[8][16];
    int n = 0;
    if (a) { snprintf(caps[n++], 16, "%s", a); }
    if (b) { snprintf(caps[n++], 16, "%s", b); }
    qzos_services_set_app_perms(id, caps, n);
}

int main(void)
{
    /* ---- 注册表的基本性质 ---- */
    int n = qzos_services_method_count();
    ok(n > 0, "注册表非空");
    ok(qzos_services_has_method("sys.info"), "sys.info 在注册表内");
    ok(qzos_services_has_method("sys.storage.statfs"), "sys.storage.statfs 在注册表内");
    ok(!qzos_services_has_method("sys.power"), "sys.power 不在注册表内（还没实现）");
    ok(!qzos_services_has_method("fsWrite"), "裸 fsWrite 不是服务");
    ok(!qzos_services_has_method("processSpawn"), "processSpawn 不是服务");
    ok(!qzos_services_has_method(NULL), "NULL 方法名不是服务");
    ok(!qzos_services_has_method(""), "空方法名不是服务");

    /* ---- 每一项：带 cap 的必须落在自己能力的命名空间下 ----
     * 这是最重要的一条静态检查。`cap="power"` 配 `method="sys.storage"`
     * 看着只是难用；反过来 cap=NULL 配一个能碰硬件的方法才是安全问题。
     * 运行时 verify_registry() 也会查一遍，但那是启动时打日志——
     * 测试里查才能让它红。 */
    {
        static const char *probe[] = {
            "sys.info", "sys.storage.statfs", "sys.storage", "sys.settings",
            "sys.net", "sys.power", "sys.storage.list", "sys.info.extra"
        };
        for (unsigned i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
            const char *cap = qzos_services_method_cap(probe[i]);
            if (cap) {
                char ns[24];
                snprintf(ns, sizeof(ns), "sys.%s", cap);
                if (strncmp(probe[i], ns, strlen(ns)) != 0) {
                    char msg[192];
                    snprintf(msg, sizeof(msg),
                             "cap 不自洽: %s 需要 '%s' 却不在 '%s.*' 下",
                             probe[i], cap, ns);
                    ok(0, msg);
                    continue;
                }
            }
        }
        ok(1, "带能力的方法都在自己能力的命名空间下");
    }

    /* ---- 能力要求是显式的，不靠前缀匹配 ----
     * 早期实现是 `method 以 sys. 开头就算、cap 取 sys. 后那段做前缀比对`。
     * 那等于「sys. 下的任何方法名都可达」，而实际 handler 只有一个——
     * 等于给未来留了一扇没锁的门。改成注册表显式声明后，
     * sys.storage 没实现就是 not-a-service（被拒），而不是「有权限就能调」。 */
    ok(qzos_services_method_cap("sys.storage") == NULL,
       "未实现的方法没有能力声明（不会被前缀匹配误放行）");
    ok(qzos_services_method_cap("sys.info") == NULL,
       "sys.info 是纯只读元信息，无需能力");
    ok(qzos_services_method_cap("sys.storage.statfs") != NULL &&
       strcmp(qzos_services_method_cap("sys.storage.statfs"), "storage") == 0,
       "sys.storage.statfs 需要 storage 能力");

    /* ---- 缺省全拒：没设过授权时一切能力方法都不可用 ----
     * 这条是「忘记初始化授权」的安全默认值：表现为拒绝而不是放行。
     *
     * 只查「授权上下文里存了几个能力」**不够**——那查的是数据，不是判定。
     * 真正的判据是 qzos_services_would_allow()：把 caps_allow 的行为也纳入
     * 覆盖。少了这一条的话，「缺省放行」这个变异能全绿过。 */
    {
        char out[8][16];
        int got = qzos_services_app_caps(out, 8);
        ok(got == 0, "进程刚起来时授权上下文为空");
        ok(!qzos_services_would_allow("sys.storage.statfs"),
           "没设授权时 sys.storage.statfs 被拒（default-deny）");
        ok(qzos_services_would_allow("sys.info"),
           "没设授权时 sys.info 仍可用（纯只读元信息，无需能力）");
        ok(!qzos_services_would_allow("sys.power"),
           "没设授权时未注册的方法不可用");
    }

    /* ---- 授权生效后，判定随之改变 ---- */
    {
        set_caps("app", "storage", NULL);
        ok(qzos_services_would_allow("sys.storage.statfs"),
           "授了 storage 后 sys.storage.statfs 放行");
        ok(qzos_services_would_allow("sys.info"), "sys.info 一直可用");
        set_caps("app", "info", "storage");
        ok(qzos_services_would_allow("sys.storage.statfs"),
           "授了 info+storage 后放行");
        set_caps("app", "info", NULL);
        ok(!qzos_services_would_allow("sys.storage.statfs"),
           "只授 info 时 storage 仍被拒（能力不串味）");
        set_caps("app", "net", NULL);
        ok(!qzos_services_would_allow("sys.storage.statfs"),
           "授了 net 后 storage 仍被拒（能力不串味）");
        qzos_services_set_app_perms(NULL, NULL, 0);
        ok(!qzos_services_would_allow("sys.storage.statfs"),
           "back 之后立刻回到全拒");
    }

    /* ---- 表外能力整条拒绝 ----
     * 静默忽略会让应用带着残缺授权在系统里跑而作者不知情。 */
    {
        char caps[8][16];
        snprintf(caps[0], 16, "info");
        snprintf(caps[1], 16, "sudo");          /* 不在能力表 */
        ok(!qzos_services_set_app_perms("x", caps, 2), "表外能力被拒");
        char out[8][16];
        ok(qzos_services_app_caps(out, 8) == 0, "被拒后授权上下文被清空（不留半授权）");
    }
    {
        char caps[8][16];
        ok(qzos_services_set_app_perms("x", caps, 0), "零能力的应用是合法的（default-deny）");
    }
    {
        char caps[8][16];
        snprintf(caps[0], 16, "sudo");
        ok(!qzos_services_set_app_perms("x", caps, 1), "只含表外能力 → 拒");
    }

    /* ---- 授权上下文的读写 ---- */
    set_caps("notepad", "info", "storage");
    {
        char out[8][16];
        int got = qzos_services_app_caps(out, 8);
        ok(got == 2, "两个能力都记下了");
        ok(strcmp(out[0], "info") == 0 && strcmp(out[1], "storage") == 0, "能力内容正确");
    }
    qzos_services_set_app_perms(NULL, NULL, 0);   /* back */
    {
        char out[8][16];
        ok(qzos_services_app_caps(out, 8) == 0, "back 后授权清空");
    }

    /* ---- 能力数量上限：超出的不能悄悄写坏内存 ---- */
    {
        char caps[16][16];
        for (int i = 0; i < 16; i++) snprintf(caps[i], 16, "info");
        /* 16 个同名能力：qzos_services_set_app_perms 按 max=8 截断，不该崩 */
        qzos_services_set_app_perms("many", caps, 16);
        char out[8][16];
        ok(qzos_services_app_caps(out, 8) <= 8, "超量能力被截断到上限内");
    }
    qzos_services_set_app_perms(NULL, NULL, 0);

    if (failed == 0) {
        printf("OK: %d checks, 0 failed\n", checks);
    } else {
        printf("FAILED: %d of %d\n", failed, checks);
        for (int i = 0; i < nfail; i++) printf("  - %s\n", failures[i]);
    }
    return failed ? 1 : 0;
}
