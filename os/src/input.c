/*
 * input.c — evdev keyboard -> LVGL keypad indev + group focus nav
 *
 * Devices: /dev/input/event0 (matrix) + event1 (gpio), overridable via
 * QZ_INPUT0 / QZ_INPUT1 (empty string disables).
 *
 * Keycode table (C1Terminal keyboard.go):
 *   Enter=28 OK=352 Up=103 Down=108 Left=105 Right=106
 *   Home=102 Back=158 Delete=111 Space=57 letters=16..50
 *
 * Routing:
 *   arrows/Enter/OK/Delete/Backspace -> LVGL keypad (group focus + widgets)
 *   printable letters/space          -> LVGL keypad (widget text input)
 *   Home / Back / Volume / Wakeup    -> system keys -> JSON event to JS shell
 *
 * Events are queued from uv_poll callbacks; the LVGL indev read_cb pops one
 * event per read. E-ink: input-driven only, no polling timers beyond LVGL's
 * own indev read period.
 */
#include "qzos.h"

#include <lvgl.h>
#include <uv.h>

#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "keymap.h"

#define EVQ_SIZE 32

typedef struct {
    uint16_t code;
    uint8_t state; /* 1 press, 0 release, 2 repeat */
} evq_item_t;

static evq_item_t s_evq[EVQ_SIZE];
static unsigned s_evq_head, s_evq_tail;

static lv_indev_t *s_keypad;
static lv_group_t *s_group;
static uint32_t s_last_key; /* retained across release reads */
static bool s_trace;         /* QZ_INPUT_DEBUG: 每一次进/出队都打 —— 输入是
                              * 唯一没有自动化覆盖的通路，链路上任何一环
                              * 静默丢键都表现为"屏上没反应"，无法定位 */

static bool push_event(uint16_t code, uint8_t state)
{
    unsigned next = (s_evq_head + 1) % EVQ_SIZE;
    if (next == s_evq_tail) return false; /* overflow: drop */
    s_evq[s_evq_head].code = code;
    s_evq[s_evq_head].state = state;
    s_evq_head = next;
    return true;
}

static bool pop_event(evq_item_t *out)
{
    if (s_evq_tail == s_evq_head) return false;
    *out = s_evq[s_evq_tail];
    s_evq_tail = (s_evq_tail + 1) % EVQ_SIZE;
    return true;
}

/* ---- keycode routing ---- */

/* 键码 -> 按键语义的映射在 keymap.c（纯逻辑、可单测）；这里只负责把
 * 宿主语义翻成 LVGL 的 key 码。映射本身不写在��边是有原因的：字母段的
 * evdev 码不连续，在 input.c 里用算术推字符会静默打错每个字母。 */

/* 宿主按键语义 -> LVGL key */
static uint32_t lv_key_of(qzos_key_t k, char c)
{
    switch (k) {
    case QZ_KEY_UP:        return LV_KEY_UP;
    case QZ_KEY_DOWN:      return LV_KEY_DOWN;
    case QZ_KEY_LEFT:      return LV_KEY_LEFT;
    case QZ_KEY_RIGHT:     return LV_KEY_RIGHT;
    case QZ_KEY_ENTER:     return LV_KEY_ENTER;
    case QZ_KEY_ESC:       return LV_KEY_ESC;
    case QZ_KEY_BACKSPACE: return LV_KEY_BACKSPACE;
    case QZ_KEY_DEL:       return LV_KEY_DEL;
    case QZ_KEY_NEXT:      return LV_KEY_NEXT;
    case QZ_KEY_PREV:      return LV_KEY_PREV;
    case QZ_KEY_HOME:      return LV_KEY_HOME;
    case QZ_KEY_END:       return LV_KEY_END;
    case QZ_KEY_CHAR:      return (uint32_t)(unsigned char)c;
    case QZ_KEY_NONE:
    default:               return 0;
    }
}

/* 方向键 -> 焦点移动（仅非编辑态）
 *
 * 为什么必须在这里做：LVGL v9 的 keypad 只把 LV_KEY_NEXT / LV_KEY_PREV 当作
 * "移动 group 焦点"，方向键要走 gridnav 才管用，而本项目 LV_USE_GRIDNAV=0
 * （e-ink 单列 UI 用不上 grid，也不想为它引入布局约束）。于是把 evdev 方向键
 * 直接交给 LVGL 的后果是：桌面应用列表**在真机上根本走不动**——设备键盘只有
 * Enter/方向/Home/Back/OK，没有 Tab 也没有 PageUp/PageDown（见 C1Terminal
 * keyboard.go），能触发 NEXT/PREV 的键一个都没有。
 *
 * 编辑态（焦点在 textarea 上，由 bridge 的 op:focus 设置）下方向键交还给控件
 * 用于移动光标，不做这层翻译。 */
static uint32_t adapt_arrows(qzos_key_t kind, uint32_t lv)
{
    if (lv_group_get_editing(qzos_input_group())) return lv;
    switch (kind) {
    case QZ_KEY_DOWN:
    case QZ_KEY_RIGHT: return LV_KEY_NEXT;
    case QZ_KEY_UP:
    case QZ_KEY_LEFT:  return LV_KEY_PREV;
    default:           return lv;
    }
}

/* ---- LVGL keypad read ---- */

static void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    evq_item_t ev;
    while (pop_event(&ev)) {
        if (qzos_keymap_is_system(ev.code)) {
            if (ev.state == 1) {
                const char *n = qzos_keymap_system_name(ev.code);
                if (n) qzos_bridge_send_key(n);
            }
            continue;
        }
        if (ev.state == 0) { /* release of last key */
            data->key = s_last_key;
            data->state = LV_INDEV_STATE_RELEASED;
            if (s_trace) fprintf(stderr, "[in] -> LVGL release key=0x%02x\n", data->key);
            return;
        }
        /* 注意：必须分两步写。不能写成
         *     lv_key_of(qzos_keymap_decode(code, &ch), ch)
         * 函数实参的求值顺序在 C 里是未指定的，GCC 从右往左求值，于是 ch
         * 在 decode 填它之前就被读走（恒为 0），所有字符键被判成"未映射"
         * 静默丢弃——表现是"字母打不进去"，而日志里连一行都没有。 */
        char ch = 0;
        qzos_key_t kind = qzos_keymap_decode(ev.code, &ch);
        uint32_t k = adapt_arrows(kind, lv_key_of(kind, ch));
        if (!k) continue; /* unmapped */
        s_last_key = k;
        data->key = k;
        data->state = LV_INDEV_STATE_PRESSED;
        if (s_trace)
            fprintf(stderr, "[in] -> LVGL press key=0x%02x focus=%p\n",
                    k, (void *)lv_group_get_focused(s_group));
        return;
    }
    /* nothing new: hold current key state as released */
    data->key = s_last_key;
    data->state = LV_INDEV_STATE_RELEASED;
}

/* ---- evdev pumping ---- */

typedef struct {
    uv_poll_t poll;
    int fd;
} evdev_t;

static evdev_t s_ev[2];

static void poll_cb(uv_poll_t *handle, int status, int events)
{
    (void)events;
    if (status < 0) return;
    evdev_t *dev = (evdev_t *)handle;
    struct input_event ie[16];
    for (;;) {
        ssize_t n = read(dev->fd, ie, sizeof(ie));
        if (n < (ssize_t)sizeof(ie[0])) break; /* EAGAIN / short read */
        size_t cnt = (size_t)n / sizeof(ie[0]);
        for (size_t i = 0; i < cnt; i++) {
            if (ie[i].type != EV_KEY) continue;
            uint16_t code = ie[i].code;
            uint8_t st = ie[i].value == 2 ? 2 : (uint8_t)ie[i].value;
            if (st == 2) {
                /* 长按连发只对导航键有意义：字母连发会往 textarea 里
                 * 灌一串重复字符。 */
                if (!qzos_keymap_repeatable(code)) continue;
                st = 1;
            }
            if (!push_event(code, st) && s_trace)
                fprintf(stderr, "[in] evq OVERFLOW dropped code=%u\n", code);
            else if (s_trace)
                fprintf(stderr, "[in] ev code=%u state=%u\n", code, st);
        }
    }
}

static int open_evdev(const char *path, uv_loop_t *loop, evdev_t *slot)
{
    if (!path || !*path) return 0;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "qzos-input: cannot open %s (skipped)\n", path);
        return 0; /* not fatal: e.g. no event1 on some units / qemu */
    }
    slot->fd = fd;
    uv_poll_init(loop, &slot->poll, fd);
    slot->poll.data = slot;
    uv_poll_start(&slot->poll, UV_READABLE, poll_cb);
    fprintf(stderr, "qzos-input: %s\n", path);
    return 1;
}

lv_group_t *qzos_input_group(void)
{
    return s_group;
}

int qzos_input_init(uv_loop_t *loop)
{
    s_trace = getenv("QZ_INPUT_DEBUG") != NULL;

    s_group = lv_group_create();
    lv_group_set_default(s_group);

    s_keypad = lv_indev_create();
    lv_indev_set_type(s_keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_keypad, keypad_read_cb);
    lv_indev_set_group(s_keypad, s_group);

    const char *p0 = getenv("QZ_INPUT0");
    const char *p1 = getenv("QZ_INPUT1");
    open_evdev(p0 ? p0 : "/dev/input/event0", loop, &s_ev[0]);
    open_evdev(p1 ? p1 : "/dev/input/event1", loop, &s_ev[1]);
    if (s_trace)
        fprintf(stderr, "[in] group=%p indev=%p dev0=%s dev1=%s "
                        "disp=%p next_timer=%ums\n",
                (void *)s_group, (void *)s_keypad,
                p0 ? p0 : "/dev/input/event0", p1 ? p1 : "(default)",
                (void *)(s_keypad ? lv_indev_get_display(s_keypad) : NULL),
                lv_timer_get_time_to_next());
    return 0;
}
