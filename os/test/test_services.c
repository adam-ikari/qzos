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

#include "appauth.h"

#include <stdio.h>
#include <stdlib.h>
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

/* ---- fixture：能力只能通过磁盘 manifest 进来 ----
 *
 * 这里没有「set_caps(id, caps[])」这种helper 可用了，而且**这正是重点**：
 * 曾经唯一的入口就是那个函数，而它由 JS 消息灌入，等于把授权决定权交给
 * 攻击者。现在每个 fixture 都必须在磁盘上真的写一个 app.json。 */
static char g_manifest_root[256];

static void write_manifest(const char *dir, const char *json)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", g_manifest_root, dir);
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", p);
    if (system(cmd) != 0) { fprintf(stderr, "mkdir failed: %s\n", cmd); exit(1); }
    snprintf(p, sizeof(p), "%s/%s/app.json", g_manifest_root, dir);
    FILE *f = fopen(p, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", p); exit(1); }
    fputs(json, f);
    fclose(f);
}

/* 写一个合法 manifest（id 与目录名一致）并声明为当前应用。 */
static void set_caps(const char *dir, const char *a, const char *b)
{
    char json[512];
    if (b) snprintf(json, sizeof(json),
        "{\"schema\":1,\"id\":\"%s\",\"version\":\"1\",\"api\":1,"
        "\"entry\":\"a.js\",\"perms\":[\"%s\",\"%s\"]}", dir, a, b);
    else if (a) snprintf(json, sizeof(json),
        "{\"schema\":1,\"id\":\"%s\",\"version\":\"1\",\"api\":1,"
        "\"entry\":\"a.js\",\"perms\":[\"%s\"]}", dir, a);
    else snprintf(json, sizeof(json),
        "{\"schema\":1,\"id\":\"%s\",\"version\":\"1\",\"api\":1,"
        "\"entry\":\"a.js\",\"perms\":[]}", dir);
    write_manifest(dir, json);
    qzos_services_note_app(dir);
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
    /* ---- fixture：建一批磁盘 manifest ----
     * 全部在临时目录里，测完由 trap 之外的手工清理（/tmp 下，留着无害）。
     * 每个 fixture 都是一种攻击写法或一种合法形状，用名字标出来。 */
    {
        char tmpl[64];
        snprintf(tmpl, sizeof(tmpl), "/tmp/qzos-apputil-XXXXXX");
        if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 1; }
        snprintf(g_manifest_root, sizeof(g_manifest_root), "%s", tmpl);
        write_manifest("full",     "{\"schema\":1,\"id\":\"full\",\"version\":\"1\",\"api\":1,\"entry\":\"a.js\",\"perms\":[\"info\",\"storage\"]}");
        write_manifest("none",     "{\"schema\":1,\"id\":\"none\",\"version\":\"1\",\"api\":1,\"entry\":\"a.js\",\"perms\":[]}");
        write_manifest("noperms",  "{\"schema\":1,\"id\":\"noperms\",\"version\":\"1\",\"api\":1,\"entry\":\"a.js\"}");
        write_manifest("imposter", "{\"schema\":1,\"id\":\"victim\",\"version\":\"1\",\"api\":1,\"entry\":\"a.js\",\"perms\":[\"storage\",\"power\"]}");
        write_manifest("sudoer",   "{\"schema\":1,\"id\":\"sudoer\",\"version\":\"1\",\"api\":1,\"entry\":\"a.js\",\"perms\":[\"info\",\"sudo\"]}");
        write_manifest("broken",   "{ this is not json ");
    }

    /* ---- appauth：id 能否当单级目录名用 ----
     *
     * 这一层挡的是路径穿越。它必须在**拼路径之前**判断，拼接后 realpath 检查
     * 太晚（`a/../../etc` 拼出来看着是绝对路径，但检查点已经晚了）。
     * 每一条拒绝都有一个具体的攻击写法在背后，所以逐条列出。 */
    {
        ok(qzos_apputil_id_is_safe("notepad"), "普通 id 合法");
        ok(qzos_apputil_id_is_safe("a-b_c.1"), "字母数字 - _ . 合法");
        ok(!qzos_apputil_id_is_safe(NULL), "NULL id");
        ok(!qzos_apputil_id_is_safe(""), "空串 id");
        ok(!qzos_apputil_id_is_safe(".."), "'..' 是穿越的钥匙");
        ok(!qzos_apputil_id_is_safe("."), "'.' 是穿越的钥匙");
        ok(!qzos_apputil_id_is_safe(".hidden"), "前导 '.' 一律拒");
        ok(!qzos_apputil_id_is_safe("a/b"), "'/' 可跨级");
        ok(!qzos_apputil_id_is_safe("../../etc"), "'../..' 是最典型的穿越写法");
        ok(!qzos_apputil_id_is_safe("a\\b"), "反斜杠（不是 POSIX 分隔符，但挡掉省得论证）");
        ok(!qzos_apputil_id_is_safe("a b"), "空格");
        ok(!qzos_apputil_id_is_safe("a;b"), "分号");
        {
            char longid[128];
            memset(longid, 'a', sizeof(longid) - 1);
            longid[sizeof(longid) - 1] = '\0';
            ok(!qzos_apputil_id_is_safe(longid), "超长 id（>63）");
        }
    }

    /* ---- appauth：能力只从磁盘 manifest 推导 ----
     *
     * 这是提权漏洞的修复本体。曾经 op:app 带着 JS 侧填的 perms 数组直接
     * 当授权收下，而应用与 shell 共享同一个 QuickJS 全局、`ui` 是全局对象，
     * 于是任何应用都能 `ui.setApp('self', ['storage'])` 给自己授权。
     * 端到端证据见 scripts/test-appauth.sh；这里测规则。 */
    {
        char caps[8][16];
        int n = -1;
        char root[256];
        snprintf(root, sizeof(root), "%s", g_manifest_root);

        /* 0. 受信根没设 ⇒ 任何 id 都拿不到能力。fail-closed 的第一道。 */
        qzos_apputil_set_apps_root(NULL);
        ok(!qzos_apputil_caps_from_manifest("full", caps, 8, &n), "没设受信根 → 拒");
        ok(n == 0, "没设受信根 → 零能力");
        qzos_apputil_set_apps_root(root);

        /* 1. 正常 manifest：perms 照抄（表内的） */
        ok(qzos_apputil_caps_from_manifest("full", caps, 8, &n), "正常 manifest 解析成功");
        ok(n == 2, "两个能力");
        ok(strcmp(caps[0], "info") == 0 && strcmp(caps[1], "storage") == 0, "能力内容与磁盘一致");

        /* 2. 声明零能力是合法结果，不是错误 */
        ok(qzos_apputil_caps_from_manifest("none", caps, 8, &n), "零能力 manifest 解析成功");
        ok(n == 0, "零能力 manifest → 0 个能力");

        /* 3. perms 字段整个缺失 */
        ok(qzos_apputil_caps_from_manifest("noperms", caps, 8, &n), "缺 perms 字段 → 成功但零能力");
        ok(n == 0, "缺 perms 字段 → 0 个能力");

        /* 4. 冒名：目录名与 manifest 里的 id 不一致 → 整条拒。
         *    少了这条，把 victim 的 manifest 拷进自己目录就能继承它的授权。 */
        ok(!qzos_apputil_caps_from_manifest("imposter", caps, 8, &n), "id 与目录名不符 → 拒");
        ok(n == 0, "冒名 → 零能力（不是沿用磁盘上别处的授权）");

        /* 5. 表外能力：整条 manifest 作废，不部分放行 */
        ok(!qzos_apputil_caps_from_manifest("sudoer", caps, 8, &n), "表外能力 → 拒");
        ok(n == 0, "表外能力 → 零能力（不是只保留表内那几个）");

        /* 6. 读不到就是读不到。绝不猜、绝不沿用上一次的授权。 */
        ok(!qzos_apputil_caps_from_manifest("ghost", caps, 8, &n), "manifest 不存在 → 拒");
        ok(n == 0, "manifest 不存在 → 零能力");
        ok(!qzos_apputil_caps_from_manifest("", caps, 8, &n), "空 id → 拒");

        /* 7. 穿越写法即使「看起来」落在受信根内也拒 */
        ok(!qzos_apputil_caps_from_manifest("../full", caps, 8, &n), "'../full' → 拒");
        ok(!qzos_apputil_caps_from_manifest("a/b", caps, 8, &n), "'a/b' → 拒");

        /* 8. 解析失败之后，上一个应用的授权不能漏过来。
         *    这是「静默给错」的另一种形状：不是给多，是给错的那个。 */
        ok(qzos_apputil_caps_from_manifest("full", caps, 8, &n), "先成功一次（2 能力）");
        ok(n == 2, "此时确有 2 个能力");
        ok(!qzos_apputil_caps_from_manifest("ghost", caps, 8, &n), "再解析一个不存在的");
        ok(n == 0, "失败后计数被清零，不是沿用上一次的 2");
    }

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
        qzos_services_note_app(NULL);
        ok(!qzos_services_would_allow("sys.storage.statfs"),
           "back 之后立刻回到全拒");
    }

    /* ---- 表外能力整条拒绝 ----
     * 静默忽略会让应用带着残缺授权在系统里跑而作者不知情。
     * 注意判据是「**当前应用的授权上下文**」，不是某个函数返回值 ——
     * 能力现在只能从磁盘 manifest 来，而 manifest 读失败时没有返回值可看，
     * 唯一的可观测结果就是「这个应用一条能力都没有」。 */
    {
        /* perms 里混一个表外的 sudo：整条作废，连 info 也不给 */
        write_manifest("mixed", "{\"schema\":1,\"id\":\"mixed\",\"version\":\"1\","
                                "\"api\":1,\"entry\":\"a.js\","
                                "\"perms\":[\"info\",\"sudo\"]}");
        qzos_services_note_app("mixed");
        char out[8][16];
        ok(qzos_services_app_caps(out, 8) == 0,
           "表外能力 → 整条 manifest 作废（不是只保留表内的 info）");
    }
    {
        /* 只有表外能力 */
        write_manifest("sudoer2", "{\"schema\":1,\"id\":\"sudoer2\",\"version\":\"1\","
                                  "\"api\":1,\"entry\":\"a.js\","
                                  "\"perms\":[\"sudo\"]}");
        qzos_services_note_app("sudoer2");
        char out[8][16];
        ok(qzos_services_app_caps(out, 8) == 0, "只含表外能力 → 零能力");
        ok(!qzos_services_would_allow("sys.storage.statfs"), "表外能力不带来任何权限");
    }
    {
        /* 零能力是合法结果 */
        set_caps("zerocap", NULL, NULL);
        char out[8][16];
        ok(qzos_services_app_caps(out, 8) == 0, "零能力的应用是合法的（default-deny）");
        ok(!qzos_services_would_allow("sys.storage.statfs"), "零能力应用调不动需要能力的方法");
    }

    /* ---- 授权上下文的读写（唯一入口：note_app + 磁盘 manifest）---- */
    set_caps("notepad", "info", "storage");
    {
        char out[8][16];
        int got = qzos_services_app_caps(out, 8);
        ok(got == 2, "两个能力都记下了");
        ok(strcmp(out[0], "info") == 0 && strcmp(out[1], "storage") == 0, "能力内容正确");
    }
    qzos_services_note_app(NULL);   /* back */
    {
        char out[8][16];
        ok(qzos_services_app_caps(out, 8) == 0, "back 后授权清空");
    }

    /* ---- 数量上限：manifest 写超量能力不能悄悄写坏内存 ---- */
    {
        write_manifest("many", "{\"schema\":1,\"id\":\"many\",\"version\":\"1\","
                               "\"api\":1,\"entry\":\"a.js\",\"perms\":["
                               "\"info\",\"info\",\"info\",\"info\",\"info\","
                               "\"info\",\"info\",\"info\",\"info\",\"info\","
                               "\"info\",\"info\"]}");
        qzos_services_note_app("many");
        char out[8][16];
        ok(qzos_services_app_caps(out, 8) <= 8, "超量能力被截断到上限内");
    }
    qzos_services_note_app(NULL);

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
