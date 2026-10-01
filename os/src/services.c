/*
 * services.c — 系统服务面（brain: qzos-service-boundary）
 *
 * **这是 JS 碰到 C 的唯一通道。** `s_registered[]` 里的方法集合 = C 能力的
 * 完整清单；清单之外没有第二条路到 C。
 *
 * 与渲染桥（bridge.c）的关系：渲染桥是**纯命令通道**（create/set/del/on/
 * clear/focus/refresh），不承载能力。能力只经由本文件。
 *
 * 所以授权检查必须落在这里（`qzos_services_set_app_perms`）——落在渲染桥上是
 * 历史偶然：早期只有 sys.info 一个方法，op_rpc 顺手就检查了；后来加了能力
 * 才暴露出那个位置不对。挂错位置的检查等于没有检查：它只挡住了那一条通道。
 *
 * 传输：
 *   inproc://qzos        — 宿主内模块 + JS 桥客户端（零拷贝）
 *   ipc://<QZ_RPC_SOCK>  — 外部服务进程（默认 /storage/qzos/rpc.sock）
 */
#include "qzos.h"

#include "appauth.h"

#include <uvrpc.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static uvrpc_server_t *s_local;
static uvrpc_server_t *s_ipc;
static uvrpc_client_t *s_local_client;
static uvbus_loop_registry_t *s_registry; /* required for INPROC peers */
static char s_ipc_addr[256];

/* ---- 当前应用授权（缺省为空 = 全拒）----
 *
 * 「缺省为空」是关键不变量：宿主启动后、shell 发来 op:app 之前，任何 rpc 都
 * 应当被拒。这让「忘记初始化授权」表现为**拒绝**而不是**放行**。
 */
/* 能力数组本身住在 appauth 推导结果里。已知能力表也搬去了 appauth.c ——
 * 判定与「表是什么」必须在同一个文件，否则加一个能力要记得改两处，
 * 而漏改的后果是「新能力谁都拿不到」或「旧表把新方法当表外」。 */
#define MAX_CAPS 8
static char s_app_caps[MAX_CAPS][16];
static int  s_app_n_caps;
static bool s_app_active;   /* 0 = 桌面/未声明 ⇒ 缺省全拒 */

/* ---- 方法注册表：C 能力的完整清单 ----
 *
 * 收口完成的判据是「这张表之外无 C 能力」，所以它必须**穷举**，而不是
 * 「按前缀匹配 sys.* 就放行」。前缀式匹配曾让 `sys.` 下的任何方法名都可达，
 * 而实际 handler 只有一个——那等于给未来留了一扇没锁的门。
 */
typedef struct {
    const char *method;
    const char *cap;      /* 该方法需要的能力；NULL = 无需授权（仅 sys.*）*/
    void (*handler)(uvrpc_request_t *req, void *ctx);
} svc_entry_t;

static void sys_info_handler(uvrpc_request_t *req, void *ctx);
static void sys_storage_statfs_handler(uvrpc_request_t *req, void *ctx);

static const svc_entry_t s_registered[] = {
    /* 方法名          所需能力     handler */
    { "sys.info",      NULL,        sys_info_handler },
    { "sys.storage.statfs", "storage", sys_storage_statfs_handler },
};
#define N_REGISTERED ((int)(sizeof(s_registered) / sizeof(s_registered[0])))

/* 每个方法是否**真的**绑到了 inproc server 上。
 *
 * 这不是冗余记账，是补一个静默失败：uvrpc 的 client 恒把 status 填成 OK
 * （third_party/uvrpc/src/uvrpc_client.c:158），而 server 对「handler 不存在」
 * 是把 int32 错误码塞进 result 的头 4 字节（uvrpc_server.c:207）。所以一旦
 * 注册失败，本仓的调用方**看不出任何异常**——只会收到 ok:true 加一坨二进制。
 *
 * 而注册表是我们自己的唯一真相源：svc_find 会说「有这个方法」，uvrpc 那边却
 * 没有 handler，调用就进了空洞。所以派发前必须核这一位，把静默的假成功换成
 * 明确的 service-not-bound。 */
static bool s_bound[N_REGISTERED];

/* 能力检查：能力 X 授予 sys.X 与 sys.X.*。
 *
 * 保守实现：只在**注册表里显式声明了 cap** 的方法上按能力判定，其余（cap==NULL，
 * 即 sys.info 这类纯只读元信息）无条件放行。新增带能力的方法时必须在这里声明，
 * 否则它会当成「无需授权」而被放行——所以下面 may_registered_cap 有一条断言：
 * 带 cap 的方法名必须落在自己能力的命名空间里。 */
static bool caps_allow(const char *cap)
{
    if (!s_app_active) return false;      /* 缺省全拒 */
    for (int i = 0; i < s_app_n_caps; i++) {
        if (strcmp(s_app_caps[i], cap) == 0) return true;
    }
    return false;
}

static int svc_find_idx(const char *method)
{
    for (int i = 0; i < N_REGISTERED; i++) {
        if (strcmp(s_registered[i].method, method) == 0) return i;
    }
    return -1;
}

static const svc_entry_t *svc_find(const char *method)
{
    int i = method ? svc_find_idx(method) : -1;
    return (i >= 0) ? &s_registered[i] : NULL;
}

/* ---- JS 侧 RPC 入口（bridge.c 转发到这里）---- */

typedef struct {
    qzos_rpc_done_t done;
    void *u;
} rpc_call_t;

static void rpc_cb(uvrpc_response_t *resp, void *ctx)
{
    rpc_call_t *c = (rpc_call_t *)ctx;
    if (resp->status == UVRPC_OK) {
        c->done(1, (const char *)resp->result, resp->result_size, c->u);
    } else {
        char err[64];
        int n = snprintf(err, sizeof(err), "{\"error\":\"rpc status %d\"}", (int)resp->status);
        c->done(0, err, (size_t)n, c->u);
    }
    free(c);
}

void qzos_services_rpc(const char *method, const char *params_json,
                       qzos_rpc_done_t done, void *u)
{
    /* 1. 授权：能力不足就地拒绝，**不进 uvrpc**。这是收口后的唯一一道检查。 */
    int idx = method ? svc_find_idx(method) : -1;
    const svc_entry_t *e = (idx >= 0) ? &s_registered[idx] : NULL;
    if (!e) {
        /* 未知方法：显式拒绝并说明「不在注册表内」。这比让它走到 uvrpc 再失败
         * 好——后者会把「我拼错了方法名」和「有权限但服务不存在」混成一个回执，
         * 而这两者的排查方向完全不同。 */
        char err[160];
        int n = snprintf(err, sizeof(err),
                         "{\"error\":\"no such service\",\"method\":\"%s\"}", method);
        done(0, err, (size_t)n, u);
        return;
    }
    /* 表里有、uvrpc 里没绑上 ⇒ 明确失败。不能派发进空洞：uvrpc 的 client 恒把
     * status 填成 OK，所以空洞里的调用会返回「成功」+ 一坨二进制错误负载，
     * 排查时看到的是一个莫名其妙的成功。 */
    if (!s_bound[idx]) {
        char err[160];
        int n = snprintf(err, sizeof(err),
                         "{\"error\":\"service not bound\",\"method\":\"%s\"}", method);
        done(0, err, (size_t)n, u);
        return;
    }
    /* 判定走 qzos_services_would_allow()，不内联 caps_allow：单测测的是前者，
     * 两处写同一规则的话，前者可以完全正确而执行点用着另一份——变异 1 就是
     * 这么溜过去的（38 条单测全绿，只有端到端抓到）。 */
    if (e->cap && !qzos_services_would_allow(method)) {
        char err[192];
        int n = snprintf(err, sizeof(err),
                         "{\"error\":\"permission denied\",\"method\":\"%s\","
                         "\"need\":\"%s\"}", method, e->cap);
        done(0, err, (size_t)n, u);
        return;
    }

    if (!s_local_client) {
        done(0, "{\"error\":\"rpc not ready\"}", 22, u);
        return;
    }
    rpc_call_t *c = malloc(sizeof(*c));
    if (!c) { done(0, "{\"error\":\"oom\"}", 16, u); return; }
    c->done = done;
    c->u = u;
    const uint8_t *params = (const uint8_t *)(params_json ? params_json : "");
    size_t len = params_json ? strlen(params_json) : 0;
    if (uvrpc_client_call(s_local_client, method, params, len, rpc_cb, c) != UVRPC_OK) {
        free(c);
        done(0, "{\"error\":\"rpc call rejected\"}", 26, u);
    }
}

/* ---- 授权上下文：**只从磁盘 manifest 推导**（brain: qzos-service-boundary）----
 *
 * 这里曾经有个公开的 qzos_services_set_app_perms(id, caps, n)，由 op:app 把
 * JS 传来的 perms 数组灌进来。那是**完整的提权漏洞**：应用与 shell 共享同一个
 * QuickJS 全局，`ui` 是全局对象，于是任何应用都能调
 * `ui.setApp('self', ['storage'])` 给自己授权。实测 perms: [] 的应用真的
 * 调通了 sys.storage.statfs。
 *
 * 授权判定放在服务面（对），但**判定的输入由攻击者提供**（错）。所以现在：
 *   - set_app_perms 不再是公开面的一部分，消息里带什么 perms 一律不看；
 *   - 唯一的合法来源是 appauth.c 从 <apps-root>/<id>/app.json 重新推导。
 *
 * JS 可以点名一个应用，不能决定它能做什么。
 */
static void set_caps_from_disk(const char *id)
{

    s_app_n_caps = 0;
    s_app_active = 0;

    if (!id) return;                    /* back / 桌面：清空，回到缺省全拒 */

    int n = 0;
    char caps[MAX_CAPS][16];
    /* 解析失败也照样进入「已激活、零能力」状态：区别在于此时一条能力都没有，
     * 而不是沿用上一个应用的授权。 */
    (void)qzos_apputil_caps_from_manifest(id, caps, MAX_CAPS, &n);
    for (int i = 0; i < n && i < MAX_CAPS; i++)
        snprintf(s_app_caps[i], sizeof(s_app_caps[0]), "%s", caps[i]);
    s_app_n_caps = n;
    s_app_active = 1;
    fprintf(stderr, "qzos-services: app '%s' authorized (%d caps, from manifest)\n",
            id, s_app_n_caps);
}

/* op:app 的唯一入口。**id 之外的一切参数都不看。** */
void qzos_services_note_app(const char *id)
{
    set_caps_from_disk(id);
}

int qzos_services_app_caps(char out[][16], int max)
{
    int n = s_app_n_caps < max ? s_app_n_caps : max;
    for (int i = 0; i < n; i++) {
        /* memcpy 而非 snprintf：源与目标都是 16 字节，编译器无法从
         * s_app_caps[i]（char[16]）推断长度，会对 snprintf 报截断告警。
         * 显式限定长度即可。 */
        size_t l = 0;
        while (l + 1 < 16 && s_app_caps[i][l]) l++;
        memcpy(out[i], s_app_caps[i], l);
        out[i][l] = '\0';
    }
    return n;
}

/* ---- built-in services ---- */

static void sys_info_handler(uvrpc_request_t *req, void *ctx)
{
    (void)ctx;
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "{\"service\":\"sys\",\"version\":\"0.2.0\",\"pid\":%d,\"uptime_s\":%ld}",
                     (int)getpid(), (long)time(NULL));
    uvrpc_request_send_response(req, UVRPC_OK, (const uint8_t *)buf, (size_t)n);
}

/* sys.storage.statfs — 存储状态。
 *
 * 这是第一个「真的碰硬件/OS」的服务，所以它是收口是否真的收住了的试金石：
 * 需要 storage 能力；不需要时在 qzos_services_rpc 就被拒，不会走到这里。
 */
static void sys_storage_statfs_handler(uvrpc_request_t *req, void *ctx)
{
    (void)ctx;
    char buf[768];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "{\"mounts\":[");
    /* 只报两个固定挂载点：rootfs（只读）与 /storage（可写，用户数据所在）。
     * 报全量 /proc/mounts 会把一堆与系统无关的 tmpfs 也带出来，对应用没用。 */
    static const char *paths[] = { "/", "/storage" };
    int first = 1;
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        struct statvfs vfs;
        if (statvfs(paths[i], &vfs) != 0) continue;
        uint64_t total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
        uint64_t avail = (uint64_t)vfs.f_bavail * vfs.f_frsize;
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "%s{\"path\":\"%s\",\"total\":%llu,\"avail\":%llu,"
                      "\"ro\":%d}",
                      first ? "" : ",", paths[i],
                      (unsigned long long)total, (unsigned long long)avail,
                      (vfs.f_flag & ST_RDONLY) ? 1 : 0);
        first = 0;
    }
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "]}");
    uvrpc_request_send_response(req, UVRPC_OK, (const uint8_t *)buf, (size_t)n);
}

/* ---- 注册表自检 ----
 *
 * 「带 cap 的方法必须落在自己能力的命名空间里」——否则 `cap="power"` 配
 * `method="sys.storage"` 这种错配会让一个 storage 方法要求 power 能力，
 * 看起来只是难用；反过来 `cap=NULL` 配一个能碰硬件的方法才是安全问题。
 * 启动时查一次，代价为零。
 */
static void verify_registry(void)
{
    for (int i = 0; i < N_REGISTERED; i++) {
        const svc_entry_t *e = &s_registered[i];
        if (!e->cap) continue;
        char ns[16];
        snprintf(ns, sizeof(ns), "sys.%s", e->cap);
        if (strncmp(e->method, ns, strlen(ns)) != 0) {
            fprintf(stderr,
                    "qzos-services: REGISTRY BUG: '%s' needs cap '%s' but is not "
                    "under namespace '%s' — the cap is a lie\n",
                    e->method, e->cap, ns);
        }
    }
}

/* ---- 生命周期 ---- */

/* 授权判定（给闸门用的可观测钩子）。
 *
 * default-deny 是这一层最关键的不变量，而它此前**没有任何直接测试**：测试能查
 * 「授权上下文里存了几个能力」，却查不到「没设授权时调用会不会被拒」，因为
 * caps_allow() 是 static。于是「缺省放行」这个变异能全绿过——靠变异测试才发现
 * 的又一次「看起来在测、其实没测到点上」。
 *
 * 暴露它是为了让那条不变量可测。语义：不在注册表 ⇒ 无此能力（false）；
 * 无需授权的只读元信息 ⇒ true；其余按当前授权上下文判定。 */
bool qzos_services_would_allow(const char *method)
{
    const svc_entry_t *e = method ? svc_find(method) : NULL;
    if (!e) return false;
    if (!e->cap) return true;
    return caps_allow(e->cap);
}

/* 给闸门用的查询口。收口完成的判据是「注册表之外无 C 能力」，而这个判据
 * 必须能从仓外验证——所以把注册表暴露成可查询的，而不是让测试去猜。 */
/* IPC 公开面的判定：cap==NULL 者才公开。与 qzos_services_init 里注册时的
 * 过滤是同一条规则，但单独暴露出来是为了让闸门能核对「注册时过滤的和这里
 * 判定的是同一批」——两处写同一规则时，那正是会漂移的地方。 */
bool qzos_services_ipc_exposes(const char *method)
{
    const svc_entry_t *e = method ? svc_find(method) : NULL;
    return e && e->cap == NULL;
}

int qzos_services_ipc_exposed_count(void)
{
    int n = 0;
    for (int i = 0; i < N_REGISTERED; i++) {
        if (!s_registered[i].cap) n++;
    }
    return n;
}

int qzos_services_method_count(void)
{
    return N_REGISTERED;
}

/* 未真正绑到 inproc server 上的方法数。闸门在起过宿主之后断言它是 0。
 * 之前注册失败只打一行日志，而调用方**收不到任何异常**（见 s_bound 的注释），
 * 所以这一位必须能被测，不能只靠人看日志。 */
int qzos_services_unbound_count(void)
{
    int n = 0;
    for (int i = 0; i < N_REGISTERED; i++) {
        if (!s_bound[i]) n++;
    }
    return n;
}

bool qzos_services_has_method(const char *method)
{
    return method && svc_find(method) != NULL;
}

const char *qzos_services_method_cap(const char *method)
{
    const svc_entry_t *e = method ? svc_find(method) : NULL;
    return e ? e->cap : NULL;
}

const char *qzos_services_addr(void)
{
    return s_ipc_addr;
}

int qzos_services_init(uv_loop_t *loop)
{
    verify_registry();

    /* INPROC peers must share an explicit loop registry (uvrpc examples) */
    s_registry = uvbus_loop_registry_new();

    /* in-host listener + client */
    uvrpc_config_t *lc = uvrpc_config_new();
    uvrpc_config_set_loop(lc, loop);
    uvrpc_config_set_address(lc, "inproc://qzos");
    uvrpc_config_set_transport(lc, UVBUS_TRANSPORT_INPROC);
    uvrpc_config_set_loop_registry(lc, s_registry);
    s_local = uvrpc_server_create(lc);
    if (s_local) {
        for (int i = 0; i < N_REGISTERED; i++) {
            bool okb = uvrpc_server_register(s_local, s_registered[i].method,
                                             s_registered[i].handler, NULL) == UVRPC_OK;
            s_bound[i] = okb;
            if (!okb)
                fprintf(stderr, "qzos-services: register '%s' failed\n",
                        s_registered[i].method);
        }
        if (uvrpc_server_start(s_local) != UVRPC_OK) {
            fprintf(stderr, "qzos-services: local server start failed\n");
            s_local = NULL;
        }
    }
    uvrpc_config_free(lc);

    if (s_local) {
        uvrpc_config_t *cc = uvrpc_config_new();
        uvrpc_config_set_loop(cc, loop);
        uvrpc_config_set_address(cc, "inproc://qzos");
        uvrpc_config_set_transport(cc, UVBUS_TRANSPORT_INPROC);
        uvrpc_config_set_loop_registry(cc, s_registry);
        s_local_client = uvrpc_client_create(cc);
        uvrpc_client_connect(s_local_client);
        uvrpc_config_free(cc);
    }

    /* external IPC listener (optional path: QZ_RPC_SOCK, default on /storage)
     *
     * **只注册 cap==NULL 的方法。** 这不是省略，是边界。
     *
     * 授权检查住在 qzos_services_rpc()，那是 INPROC 路径（JS 桥用的）。IPC
     * 路径若把 handler 直接注册进去，就**完全绕过**授权——实测过：宿主里一个
     * 应用都没跑，一个外部进程连上 socket 就调通了 sys.storage.statfs。
     *
     * 那条路没法补授权：unix socket 上没有可用的调用方身份。这台设备是
     * 单用户 root 盒（uid 区分不出谁是谁），所以诚实的做法是划清暴露面——
     * IPC = **公开面**（无需能力的只读元信息），INPROC = **特权面**（受应用
     * perms 约束）。要能力的方法一律不上 IPC。
     *
     * 顺带收紧 socket 权限：uvrpc 默认建出 0755，设备上 /storage 是 0777，
     * 于是「任何应用都能连」。降到 0600，让公开面也不至于谁都能连。 */
    const char *sock = getenv("QZ_RPC_SOCK");
    if (!sock) sock = "/storage/qzos/rpc.sock";
    if (*sock && strcmp(sock, "none") != 0) {
        snprintf(s_ipc_addr, sizeof(s_ipc_addr), "ipc://%s", sock);
        uvrpc_config_t *ic = uvrpc_config_new();
        uvrpc_config_set_loop(ic, loop);
        uvrpc_config_set_address(ic, s_ipc_addr);
        uvrpc_config_set_transport(ic, UVBUS_TRANSPORT_IPC);
        uvrpc_config_set_max_clients(ic, 8);
        s_ipc = uvrpc_server_create(ic);
        if (s_ipc) {
            int exposed = 0, withheld = 0;
            for (int i = 0; i < N_REGISTERED; i++) {
                /* 判据走 qzos_services_ipc_exposes()，不内联 `if (cap)`。
                 *
                 * 两处写同一规则时，那正是会漂移的地方：单测能测那条查询函数，
                 * 而这里内联一份的话，查询函数可以完全正确、实际过滤却是错的
                 * （曾就这样：变异掉内联判断，35 条单测全绿）。合并成一条路径后
                 * 「规则」只有一个来源。
                 *
                 * 剩下的分工：查询函数保证**规则**（需能力 ⇒ 不公开），
                 * scripts/test-ipc-surface.sh 保证**应用**（init 真的照它过滤）。 */
                if (!qzos_services_ipc_exposes(s_registered[i].method)) {
                    /* 需能力 → 不上 IPC。日志要说出来，否则「某个服务在 IPC 上
                     * 调不通」会被当成 bug 去查，而真相是它故意不在那里。 */
                    fprintf(stderr,
                            "qzos-services: withholding '%s' from ipc "
                            "(needs cap '%s')\n",
                            s_registered[i].method, s_registered[i].cap);
                    withheld++;
                    continue;
                }
                if (uvrpc_server_register(s_ipc, s_registered[i].method,
                                           s_registered[i].handler, NULL) != UVRPC_OK)
                    fprintf(stderr, "qzos-services: register '%s' (ipc) failed\n",
                            s_registered[i].method);
                else
                    exposed++;
            }
            if (uvrpc_server_start(s_ipc) != UVRPC_OK) {
                fprintf(stderr, "qzos-services: ipc server start failed (%s)\n", s_ipc_addr);
                s_ipc = NULL;
                s_ipc_addr[0] = '\0';
            } else {
                /* socket 建出来是 0755，设备上等于「谁都能连」；收紧到 0600。 */
                if (chmod(sock, 0600) != 0)
                    fprintf(stderr, "qzos-services: cannot chmod 0600 %s: %s\n",
                            sock, strerror(errno));
                fprintf(stderr, "qzos-services: ipc listening on %s "
                                "(%d public method(s), %d withheld)\n",
                        s_ipc_addr, exposed, withheld);
            }
        }
        uvrpc_config_free(ic);
    } else {
        s_ipc_addr[0] = '\0';
    }
    fprintf(stderr, "qzos-services: %d methods registered (C capability surface)\n",
            N_REGISTERED);
    return (s_local || s_ipc) ? 0 : -1;
}
