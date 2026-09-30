/* tmp-rpc-client.c — 外部 IPC 客户端：连宿主 ipc://<sock>，调 sys.info
 *
 * 服务拓扑验证：qzos-host 是 uvrpc hub，外部服务进程走 unix socket。
 * 用法：tmp-rpc-client <sock-path> [method]
 *   uvrpc 全异步 + 单 loop NOWAIT 泵（宿主不跑 uv_run，见 services.c）。
 */
#include <uvrpc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int s_done;
static int s_ok;

static void on_resp(uvrpc_response_t *resp, void *ctx)
{
    (void)ctx;
    s_done = 1;
    s_ok = (resp->status == UVRPC_OK);
    printf("status=%d result=[%.*s]\n", (int)resp->status,
           (int)resp->result_size, (const char *)resp->result);
}

static void on_conn(int status, void *ctx)
{
    (void)ctx;
    printf("connect status=%d\n", status);
    if (status != 0) exit(2);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <sock> [method]\n", argv[0]); return 1; }
    const char *method = argc > 2 ? argv[2] : "sys.info";

    uv_loop_t loop;
    if (uv_loop_init(&loop) != 0) return 1;

    char addr[512];
    snprintf(addr, sizeof(addr), "ipc://%s", argv[1]);

    uvrpc_config_t *cfg = uvrpc_config_new();
    uvrpc_config_set_loop(cfg, &loop);
    uvrpc_config_set_address(cfg, addr);
    uvrpc_config_set_transport(cfg, UVBUS_TRANSPORT_IPC);
    uvrpc_client_t *cl = uvrpc_client_create(cfg);
    uvrpc_config_free(cfg);
    if (!cl) { fprintf(stderr, "client create failed\n"); return 1; }

    printf("connecting to %s\n", addr);
    int rc = uvrpc_client_connect_with_callback(cl, on_conn, NULL);
    if (rc != UVRPC_OK) { fprintf(stderr, "connect rc=%d\n", rc); return 1; }

    /* give the connect a pump, then issue the call */
    const char *params = "{\"from\":\"tmp-rpc-client\"}";
    int issued = 0;
    for (int i = 0; i < 200 && !s_done; i++) {
        if (!issued && i > 3) {
            int r = uvrpc_client_call(cl, method, (const uint8_t *)params,
                                      strlen(params), on_resp, NULL);
            printf("call %s rc=%d\n", method, r);
            issued = 1;
        }
        uv_run(&loop, UV_RUN_NOWAIT);
        usleep(10000); /* 10ms — NOWAIT 泵必须有真实时间让 IO 走完 */
    }
    printf(s_ok ? "IPC RPC OK\n" : "IPC RPC FAILED\n");
    return s_ok ? 0 : 3;
}
