/*
 * main.c — qzos-host super-loop
 *
 * Single thread, single uv loop (brain: qzos-ui-architecture):
 *   - LVGL tick via clock_gettime(CLOCK_MONOTONIC)
 *   - lv_timer_handler() runs from a uv timer at LV_DEF_REFR_PERIOD
 *   - qzjs ISOLATED outbox drained via per-rt mailbox + eventfd wake
 *     (M-P7: qzjs owns its rt process/threads, never calls back into the
 *     host; qz_recv_message here -> bridge translates JSON UI ops into
 *     lv_* calls)
 *   - uvrpc services share the loop (loop injection, never uv_run itself)
 *   - evdev input via uv_poll
 *
 * No animation timers of our own: display commits happen only when the UI
 * changes (e-ink constraint).
 */
#include "qzos.h"

#include <lvgl.h>
#include <qzjs/qzjs.h>
#include <uv.h>
#include <uvrpc.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uv_loop_t s_loop;
static uv_timer_t s_lv_timer;
static uv_timer_t s_exit_timer;
static qz_t *s_rt;
static int s_running = 1;
static bool s_have_exit_timer;
static uv_poll_t s_msg_poll;
static int s_msg_fd = -1;

/* LVGL tick: monotonic ms */
static uint32_t tick_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void lv_timer_cb(uv_timer_t *t)
{
    (void)t;
    static int ticks;
    if (getenv("QZ_LOOP_DEBUG") && (++ticks % 10 == 0))
        fprintf(stderr, "[lv] tick=%d\n", ticks);
    lv_timer_handler();
    qzos_display_commit();
}

/* QZ_AUTOEXIT_S=<秒>：跑固定时长后自行退出（qemu/自动化冒烟用）。
 * 不用外部 `timeout`：它给整个进程组发 SIGTERM，会连带打断宿主，
 * 看起来像 JS 运行时崩溃。 */
static void autoexit_cb(uv_timer_t *t)
{
    (void)t;
    fprintf(stderr, "qzos-host: QZ_AUTOEXIT_S reached\n");
    uv_stop(&s_loop);
}

/* JS -> host: drain qzjs per-rt mailbox (M-P7: library posts into a FIFO and
 * wakes an eventfd; it no longer calls back into the host). */
static void drain_js_messages(void)
{
    char *json = NULL;
    size_t len = 0;
    while (qz_recv_message(s_rt, &json, &len, 0) == 0) {
        if (getenv("QZ_LOOP_DEBUG")) {
            fprintf(stderr, "[loop] msg alive=%d: %.*s\n",
                    uv_loop_alive(&s_loop), (int)(len > 120 ? 120 : len), json);
        }
        qzos_bridge_handle(json, len);
        qz_free_message(json);
        json = NULL;
    }
}

/* eventfd wakeup: recv-to-empty, then read fd to EAGAIN, then recv again
 * (mailbox contract order — see qzjs.h qz_message_fd). */
static void msg_fd_cb(uv_poll_t *p, int status, int events)
{
    (void)p; (void)events;
    if (status < 0 || s_msg_fd < 0) return;
    drain_js_messages();
    uint64_t ctr;
    while (read(s_msg_fd, &ctr, sizeof(ctr)) == (ssize_t)sizeof(ctr)) { }
    drain_js_messages();
}

static void on_sigint(int sig)
{
    (void)sig;
    if (getenv("QZ_LOOP_DEBUG"))
        fprintf(stderr, "[loop] signal %d -> uv_stop\n", sig);
    s_running = 0;
    uv_stop(&s_loop);
}

static const char *BOOT_DISPATCHER =
    "globalThis.onmessage = function (e) {\n"
    "  if (typeof globalThis.__qzos_onmessage === 'function')\n"
    "    globalThis.__qzos_onmessage(e);\n"
    "};\n";

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    /* ISOLATED runtime lookup: same dir, or QZ_RT_SERVER (qemu trampoline) */
#ifdef QZ_RT_SERVER_PATH
    setenv("QZ_RT_SERVER", QZ_RT_SERVER_PATH, 0);
#endif
    const char *js_dir = getenv("QZ_JS_DIR"); /* shell bundle location */
    if (!js_dir) js_dir = "js";

    /* boot: install the onmessage dispatcher, then self-load the shell
     * bundle (shell.js) via qzjs.fs + eval; shell takes it from there. */
    static char boot_script[1024];
    const char *app_dir = getenv("QZ_APP_DIR");
    if (!app_dir) app_dir = "/storage";
    snprintf(boot_script, sizeof(boot_script),
             "%s\n"
             "globalThis.__QZ_JS_DIR = '%s';\n"
             "globalThis.__QZ_APP_DIR = '%s';\n"
             "(async function () {\n"
             "  try {\n"
             "    var src = await qzjs.fs.readFile('%s/shell.js');\n"
             "    (0, eval)(src);\n"
             "    postMessage({evt: 'ready'});\n"
             "  } catch (err) {\n"
             "    postMessage({evt: 'error', msg: String(err)});\n"
             "  }\n"
             "})();\n",
             BOOT_DISPATCHER, js_dir, app_dir, js_dir);

    if (uv_loop_init(&s_loop) != 0) {
        fprintf(stderr, "qzos-host: uv_loop_init failed\n");
        return 1;
    }

    lv_tick_set_cb(tick_cb);
    lv_init();
    if (qzos_display_init() != 0) {
        fprintf(stderr, "qzos-host: display init failed\n");
        return 1;
    }
    /* e-ink：屏幕纯白底、全局黑字（默认主题灰底 245 会被阈值化，但显式
     * 定下避免主题差异） */
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_white(), 0);
    lv_obj_set_style_text_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_set_style_radius(lv_screen_active(), 0, 0);
    qzos_input_init(&s_loop);
    qzos_services_init(&s_loop);

    uv_timer_init(&s_loop, &s_lv_timer);
    uv_timer_start(&s_lv_timer, lv_timer_cb, LV_DEF_REFR_PERIOD, LV_DEF_REFR_PERIOD);

    const char *autoexit = getenv("QZ_AUTOEXIT_S");
    if (autoexit && *autoexit) {
        int secs = atoi(autoexit);
        if (secs > 0) {
            s_have_exit_timer = true;
            uv_timer_init(&s_loop, &s_exit_timer);
            uv_timer_start(&s_exit_timer, autoexit_cb,
                           (uint64_t)secs * 1000u, 0);
        }
    }

    qz_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.initial_script = boot_script;

    s_rt = qz_create(&cfg);
    if (!s_rt) {
        fprintf(stderr, "qzos-host: qz_create failed (qzjs-rt next to binary?)\n");
        return 1;
    }
    qzos_bridge_set_rt(s_rt);

    /* M-P7: qzjs owns its rt process/threads and posts outbox messages to a
     * per-rt mailbox + eventfd; host no longer injects its loop or a
     * message_cb. Poll the wake fd on our own loop. */
    s_msg_fd = qz_message_fd(s_rt);
    if (s_msg_fd >= 0) {
        uv_poll_init(&s_loop, &s_msg_poll, s_msg_fd);
        uv_poll_start(&s_msg_poll, UV_READABLE, msg_fd_cb);
    }

    fprintf(stderr, "qzos-host: up (display=%s)\n", getenv("QZ_DISPLAY") ? getenv("QZ_DISPLAY") : "epaper");

    uv_run(&s_loop, UV_RUN_DEFAULT);
    fprintf(stderr, "[loop] run returned alive=%d\n", uv_loop_alive(&s_loop));

    /* teardown */
    if (s_msg_fd >= 0) {
        uv_poll_stop(&s_msg_poll);
        uv_close((uv_handle_t *)&s_msg_poll, NULL);
        s_msg_fd = -1;
    }
    if (s_rt) qz_destroy(s_rt);
    uv_timer_stop(&s_lv_timer);
    uv_close((uv_handle_t *)&s_lv_timer, NULL);
    if (s_have_exit_timer) {
        uv_timer_stop(&s_exit_timer);
        uv_close((uv_handle_t *)&s_exit_timer, NULL);
    }
    uv_run(&s_loop, UV_RUN_NOWAIT);
    uv_loop_close(&s_loop);
    fprintf(stderr, "qzos-host: bye\n");
    return 0;
}
