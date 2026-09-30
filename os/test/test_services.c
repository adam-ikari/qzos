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

/* 捕获 qzos_services_rpc 的回执。服务面没起时它是同步回调，所以不需要泵。 */
typedef struct {
    int *called;
    int *success;
    char *body;
    size_t cap;
} probe_ctx_t;

static void probe_done(int ok, const char *json, size_t len, void *u)
{
    probe_ctx_t *c = (probe_ctx_t *)u;
    *c->called = 1;
    *c->success = ok;
    size_t n = len < c->cap - 1 ? len : c->cap - 1;
    memcpy(c->body, json, n);
    c->body[n] = '\0';
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

    /* ---- IPC 公开面：需能力的方法一律不上 IPC ----
     *
     * 授权检查住在 qzos_services_rpc()，那是 INPROC 路径。IPC 路径没有可用的
     * 调用方身份（unix socket；设备又是单用户 root，uid 区分不出谁是谁），
     * 所以那里**补不了授权**，只能划清暴露面。
     *
     * 曾把 handler 直接注册进 IPC，于是外部进程完全绕过授权调通了
     * sys.storage.statfs——宿主里一个应用都没跑。这条断言就是那个洞的形状；
     * 效果层面（探两侧 + 正对照）由 scripts/test-ipc-surface.sh 钉。 */
    {
        int need_cap = 0, exposed = 0;
        static const char *probe[] = {
            "sys.info", "sys.storage.statfs", "sys.storage", "sys.power",
            "sys.settings", "sys.net", "fsWrite", ""
        };
        for (unsigned i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
            const char *cap = qzos_services_method_cap(probe[i]);
            if (!cap) continue;
            need_cap++;
            if (qzos_services_ipc_exposes(probe[i])) {
                exposed++;
                char msg[192];
                snprintf(msg, sizeof(msg),
                         "需 '%s' 能力的方法 '%s' 被暴露在 IPC 公开面上（授权可被绕过）",
                         cap, probe[i]);
                ok(0, msg);
            }
        }
        ok(exposed == 0, "需能力的方法一律不上 IPC");
        ok(qzos_services_ipc_exposes("sys.info"),
           "sys.info 是纯只读元信息，可以公开");
        ok(!qzos_services_ipc_exposes("fsWrite"), "表外的东西当然不在公开面上");
        ok(!qzos_services_ipc_exposes(NULL), "NULL 不在公开面上");
        ok(need_cap >= 1, "注册表里确实存在需能力的方法（否则上面几条是空的）");
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

    /* ---- 服务面没起来时，调用必须响亮失败 ----
     *
     * uvrpc 的 client 恒把 status 填成 OK（third_party/uvrpc/src/uvrpc_client.c:158），
     * 而 server 对「handler 不存在」是把 int32 错误码塞进 result 头 4 字节
     * （uvrpc_server.c:207）。线上**没有标签**能让客户端分辨两者。
     *
     * 于是「表里有、uvrpc 里没绑上」是个静默失败：svc_find 说有，uvrpc 没 handler，
     * 调用进空洞，返回「成功」+ 一坨二进制。所以派发前核 s_bound，把静默的
     * 假成功换成明确的 service not bound。
     *
     * 本单测从不调 init，所以 s_bound 全是 false——正好是这条路径。 */
    {
        static char got[256];
        probe_ctx_t ctx;
        int called = 0, success = 0;
        ctx.called = &called; ctx.success = &success; ctx.body = got; ctx.cap = sizeof(got);
        got[0] = '\0';
        qzos_services_rpc("sys.info", "{}", probe_done, &ctx);
        ok(called == 1, "服务面没起来时调用仍会有回执（不是静默挂死）");
        ok(success == 0, "服务面没起来时调用是失败（不是 ok:true + 二进制）");
        ok(strstr(got, "service not bound") != NULL,
           "回执指名 service not bound（而不是含糊的 rpc not ready）");
    }

    if (failed == 0) {
        printf("OK: %d checks, 0 failed\n", checks);
    } else {
        printf("FAILED: %d of %d\n", failed, checks);
        for (int i = 0; i < nfail; i++) printf("  - %s\n", failures[i]);
    }
    return failed ? 1 : 0;
}
