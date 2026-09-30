/*
 * services.c — system-services plane on uvrpc (brain: qzos-services-rpc)
 *
 * Two listeners on the host uv loop:
 *   inproc://qzos      — in-host modules + JS bridge client (zero copy)
 *   ipc:///storage/qzos/rpc.sock (QZ_RPC_SOCK) — external service daemons
 *
 * Handlers are plain JSON-in/JSON-out strings for v1 (FlatBuffers service
 * schemas come with each real service). Loop injection: uvrpc never runs
 * uv_run; the host super-loop pumps everything.
 */
#include "qzos.h"

#include <uvrpc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uvrpc_server_t *s_local;
static uvrpc_server_t *s_ipc;
static uvrpc_client_t *s_local_client;
static uvbus_loop_registry_t *s_registry; /* required for INPROC peers */
static char s_ipc_addr[256];

/* ---- built-in services (v1: sys.*) ---- */

static void sys_info_handler(uvrpc_request_t *req, void *ctx)
{
    (void)ctx;
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "{\"service\":\"sys\",\"version\":\"0.1.0\",\"pid\":%d,\"uptime_s\":%ld}",
                     (int)getpid(), (long)time(NULL));
    uvrpc_request_send_response(req, UVRPC_OK, (const uint8_t *)buf, (size_t)n);
}

/* ---- JS-facing async RPC (bridge calls this) ---- */

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

const char *qzos_services_addr(void)
{
    return s_ipc_addr;
}

int qzos_services_init(uv_loop_t *loop)
{
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
        uvrpc_server_register(s_local, "sys.info", sys_info_handler, NULL);
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

    /* external IPC listener (optional path: QZ_RPC_SOCK, default on /storage) */
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
            uvrpc_server_register(s_ipc, "sys.info", sys_info_handler, NULL);
            if (uvrpc_server_start(s_ipc) != UVRPC_OK) {
                fprintf(stderr, "qzos-services: ipc server start failed (%s)\n", s_ipc_addr);
                s_ipc = NULL;
                s_ipc_addr[0] = '\0';
            } else {
                fprintf(stderr, "qzos-services: ipc listening on %s\n", s_ipc_addr);
            }
        }
        uvrpc_config_free(ic);
    } else {
        s_ipc_addr[0] = '\0';
    }
    return (s_local || s_ipc) ? 0 : -1;
}
