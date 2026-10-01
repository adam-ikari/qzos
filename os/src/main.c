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

#include "appauth.h"

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

/* ---- JS 引擎崩溃恢复 ----
 *
 * qzjs-rt 是跑 JS 的**独立进程**。它挂掉时库会把一帧
 * {"type":"error","error":"main-runtime-process-exited-unexpectedly"}
 * 推进邮箱，但仅此而已：宿主自己完全健康——LVGL 继续 tick、面板上仍是完好
 * 的桌面画面、按键仍被读取。没有这层处理，用户面对的是一个「按任何键都没
 * 反应」的僵尸桌面，只能重启设备。对一台「要作为系统」的设备这是致命的，
 * 因为桌面必须同时是**可靠性的门面**。
 *
 * 这里做三件事，缺一不可：
 *   1. 认帧 + 告知（bridge.c 的 qzos_bridge_handle 负责转成 evt:rtError）
 *   2. 停掉输入：引擎死后按键毫无意义，继续响应只会骗人
 *   3. 重建 rt 并重跑 boot，带退避（rt 反复崩溃时别疯狂重启耗电）
 */

/* 崩溃后的重启退避：1s, 2s, 4s…封顶 30s。
 * 不用固定间隔是因为最危险的场景正是「一启动就崩」——固定 1s 会变成
 * 无限重启循环，把墨水屏刷满、把电池耗光，而这些刷新一点用都没有。 */
static uint32_t s_restart_delay_ms = 1000;
static const uint32_t s_restart_delay_max_ms = 30000;

static int s_restart_armed;
static uv_timer_t s_restart_timer;
static uv_timer_t s_input_timer;   /* 死引擎期间用来消抖，不做别的事 */
static bool s_have_input_timer;
static char s_boot_script[1024];
static int s_restart_count;

static void restart_cb(uv_timer_t *t);

/* rt 死亡 → 通知 JS + 停输入 + 排一次重建。
 * 由 bridge.c 在识别到 rtError 帧时调用（见 qzos_bridge_notify_rt_death）。 */
void qzos_host_on_rt_death(const char *reason)
{
    fprintf(stderr, "qzos-host: JS engine died (%s); restart in %ums\n",
            reason ? reason : "unknown", s_restart_delay_ms);

    /* 提示与停输入都要做，且必须在「已排过重启」这个早退**之前**——
     * 第二次崩溃时若提前 return，提示就不会更新倒计时，用户看到的是
     * 上一次的旧秒数，而实际退避已经翻倍了。 */
    qzos_show_rt_dead(s_restart_delay_ms);
    qzos_input_enable(0);

    if (s_restart_armed) return;   /* 已排过一次，别叠 */
    s_restart_armed = 1;
    uv_timer_init(&s_loop, &s_restart_timer);
    uv_timer_start(&s_restart_timer, restart_cb, s_restart_delay_ms, 0);
}

/* 真正重建：qz_destroy + qz_create + 重挂 poll，然后重跑 boot。
 * M-P7 契约下宿主不持有 rt 内部状态（库不回调宿主、没有 message_cb），
 * 所以销毁就是干净的，重建也不需要额外清理。 */
static int spawn_rt(void)
{
    qz_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.initial_script = s_boot_script;

    s_rt = qz_create(&cfg);
    if (!s_rt) {
        fprintf(stderr, "qzos-host: qz_create failed (qzjs-rt next to binary?)\n");
        return -1;
    }
    qzos_bridge_set_rt(s_rt);

    /* 重挂 wake fd。旧 poll 已 close（uv_close 是异步的，所以这里 uv_run
     * 排空过才重新 init，见 restart_cb 的 uv_run(&s_loop, UV_RUN_NOWAIT)）。 */
    s_msg_fd = qz_message_fd(s_rt);
    if (s_msg_fd >= 0) {
        uv_poll_init(&s_loop, &s_msg_poll, s_msg_fd);
        uv_poll_start(&s_msg_poll, UV_READABLE, msg_fd_cb);
    }
    fprintf(stderr, "qzos-host: JS engine up (restart #%d)\n", s_restart_count);
    return 0;
}

/* 退避翻倍并重排。开机失败与运行中失败共用——两者的区别只是「有没有上一代
 * rt 要收」，策略不该有别。 */
static void arm_restart_again(void)
{
    s_restart_delay_ms =
        s_restart_delay_ms * 2 > s_restart_delay_max_ms
            ? s_restart_delay_max_ms : s_restart_delay_ms * 2;
    fprintf(stderr, "qzos-host: respawn failed, next attempt in %ums\n",
            s_restart_delay_ms);
    s_restart_armed = 1;
    uv_timer_init(&s_loop, &s_restart_timer);
    uv_timer_start(&s_restart_timer, restart_cb, s_restart_delay_ms, 0);
}

static void restart_cb(uv_timer_t *t)
{
    (void)t;
    s_restart_armed = 0;
    uv_timer_stop(&s_restart_timer);
    uv_close((uv_handle_t *)&s_restart_timer, NULL);

    /* 收掉上一代的 poll 与 rt。qz_destroy 内部会等主RT 收尸（最坏 ≤2s），
     * 期间会阻塞在本回调里——这没问题：此刻屏幕上只有一条重启提示，
     * 没有交互在进行。 */
    if (s_msg_fd >= 0) {
        uv_poll_stop(&s_msg_poll);
        uv_close((uv_handle_t *)&s_msg_poll, NULL);
        s_msg_fd = -1;
    }
    if (s_rt) { qz_destroy(s_rt); s_rt = NULL; }
    /* uv_close 是异步的：必须让 loop 跑一轮把 close 回调真正处理掉，
     * 否则下面重新 uv_poll_init 到同一个 fd 位置会撞上仍在关闭的 handle。 */
    uv_run(&s_loop, UV_RUN_NOWAIT);

    s_restart_count++;
    if (spawn_rt() != 0) {
        /* 连引擎都起不来（qzjs-rt 不在？）：退避翻倍后再试。
         * 不放弃——放弃就等于「僵尸宿主」，而那正是要修的东西。 */
        arm_restart_again();
        return;
    }

    /* 引擎活着了：恢复交互，清掉提示。退避复位。 */
    s_restart_delay_ms = 1000;
    qzos_input_enable(1);
    qzos_hide_rt_dead();
    fprintf(stderr, "qzos-host: input re-enabled\n");
}

/* 开机提交按住的兜底释放。
 *
 * 正常路径是 shell 画完桌面后发 {evt:'ready'}，bridge 放开提交。但若 shell
 * 自己挂了（脚本语法错、模块读不到、rt 早死），就永远等不到 ready —— 屏会一直
 * 空白，而用户看到的设备是「黑屏/白屏」，只能拔电。所以必须有兜底：
 * 到点无条件放开，让屏上出现「LVGL 初始化后的样子」而不是什么都没有。
 * 没有这一条的话，「按住提交」这个优化就会在 shell 故障时变成故障放大器。 */
static uv_timer_t s_hold_timer;

static void hold_timeout_cb(uv_timer_t *t)
{
    (void)t;
    uv_timer_stop(&s_hold_timer);
    uv_close((uv_handle_t *)&s_hold_timer, NULL);
    fprintf(stderr, "qzos-host: shell never signalled ready; releasing display hold\n");
    qzos_display_hold(0);
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
     * bundle (shell.js) via qzjs.fs + eval; shell takes it from there.
     *
     * boot 脚本只在这里构造一次，存进 s_boot_script（文件作用域）——rt 崩溃
     * 重建时要重跑的就是它。曾经这里还有一个局部 boot_script 填完就没人读，
     * 属于重构残留的死代码：两份 boot 脚本会各自漂移，而只有一份生效。 */
    const char *app_dir = getenv("QZ_APP_DIR");
    if (!app_dir) app_dir = "/storage";

    /* 测试专用：人为拖慢 shell boot，让「首帧提交与桌面绘制撞车」这条慢路径
     * **确定性地**发生。
     *
     * 没有它的话，开机提交数在快机器上恒为 1（碰巧对），于是「去掉提交按住」
     * 这个变异也能全绿——闸门抓不住自己该抓的回归。实测那条路径只有在
     * ASan/Debug 构建下才会自然出现，而 CI 的机器时快时慢。
     * 用法：QZ_TEST_SHELL_DELAY_MS=400 */
    char delay_js[96] = "";
    {
        const char *d = getenv("QZ_TEST_SHELL_DELAY_MS");
        int ms = (d && *d) ? atoi(d) : 0;
        if (ms > 0)
            snprintf(delay_js, sizeof(delay_js),
                     "    await new Promise(function (r) { setTimeout(r, %d); });\n", ms);
    }

    snprintf(s_boot_script, sizeof(s_boot_script),
             "%s\n"
             "globalThis.__QZ_JS_DIR = '%s';\n"
             "globalThis.__QZ_APP_DIR = '%s';\n"
             "(async function () {\n"
             "  try {\n"
             "    var src = await qzjs.fs.readFile('%s/shell.js');\n"
             "%s"
             "    (0, eval)(src);\n"
             "  } catch (err) {\n"
             /* bootFailed 与 shell 自己发的 evt:error 必须分开：前者在桌面出现
              * 之前发生（用户面对纯白屏、零诊断），后者是某个应用启动失败
              * （桌面还在）。混用一个事件名会让宿主在应用启动失败时也去盖
              * 「系统故障」屏。 */
             "    postMessage({evt: 'bootFailed', msg: String(err)});\n"
             "  }\n"
             "})();\n",
             BOOT_DISPATCHER, js_dir, app_dir, js_dir, delay_js);

    if (uv_loop_init(&s_loop) != 0) {
        fprintf(stderr, "qzos-host: uv_loop_init failed\n");
        return 1;
    }

    /* 顺序要紧：lv_init() 会清掉已注册的 tick 回调（实测 lv_tick_get_cb()
     * 由非 NULL 变 NULL），而本进程没有人调 lv_tick_inc()——时间基准只来自
     * 这个回调。所以注册必须在 lv_init() **之后**。
     *
     * 之前写反了，后果是 lv_tick_get() 恒为 0：lv_timer_handler() 认为所有
     * 定时器都没到期，于是 indev 读取定时器永不触发，键盘事件在 evdev 队列
     * 里静静烂掉——屏上「按了没反应」。绘制当时看着是好的，因为 bridge 的
     * refresh op 显式调了 lv_refr_now()，绕开了定时器；这正好把坏掉的定时器
     * 子系统盖住了。 */
    lv_init();
    lv_tick_set_cb(tick_cb);
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
    /* 授权的受信根必须**在服务面起来之前**设好：能力由 appauth.c 从
     * <root>/<id>/app.json 推导，root 缺失 = 所有应用零能力（fail-closed）。
     *
     * 用 <js_dir>/apps 而不是 QZ_APP_DIR（默认 /storage）：用户可写的目录
     * 不能当授权的来源，否则「把一个 app.json 拷进 /storage 就能拿到 power」
     * 成立。内置目录与用户目录的两路信任（brain: qzos-app-package）在 C 侧
     * 就落在这里——本仓目前只有内置这一路获得能力。 */
    {
        char apps_root[512];
        snprintf(apps_root, sizeof(apps_root), "%s/apps", js_dir);
        qzos_apputil_set_apps_root(apps_root);
        fprintf(stderr, "qzos-host: trusted apps root = %s\n", apps_root);
    }
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

    /* 开机时 rt 起不来**不能退出**：设备是墨水屏一体机，宿主一退就是黑屏，
     * 用户只能等电池耗尽或物理断电。留在退避循环里，qzjs-rt 一旦就位
     * （比如 /storage 还没挂载完、文件被占）就自动起来。
     * 代价是「看起来没反应」——所以提示必须同时画出来。 */
    if (spawn_rt() != 0) {
        fprintf(stderr, "qzos-host: initial spawn failed; staying in retry loop\n");
        qzos_show_rt_dead(s_restart_delay_ms);
        qzos_input_enable(0);
        arm_restart_again();
    }

    /* 兜底：QZ_HOLD_MAX_S 秒后无条件放开提交（默认 4s）。
     * 用环境变量而不是常量，是为了让 qemu/慢机器上的测试能调短——ASan 构建
     * 里 shell boot 明显更慢，写死 4s 会让测试等很久。 */
    {
        int hold_max = 4;
        const char *hm = getenv("QZ_HOLD_MAX_S");
        if (hm && *hm) { int v = atoi(hm); if (v > 0) hold_max = v; }
        uv_timer_init(&s_loop, &s_hold_timer);
        uv_timer_start(&s_hold_timer, hold_timeout_cb,
                       (uint64_t)hold_max * 1000u, 0);
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
