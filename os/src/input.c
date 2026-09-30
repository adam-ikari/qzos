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

/* system keys go to the JS shell as JSON */
static bool is_system_key(uint16_t code)
{
    switch (code) {
    case KEY_HOME:
    case KEY_BACK:
    case KEY_VOLUMEUP:
    case KEY_VOLUMEDOWN:
    case KEY_WAKEUP:
        return true;
    default:
        return false;
    }
}

static const char *system_key_name(uint16_t code)
{
    switch (code) {
    case KEY_HOME:       return "home";
    case KEY_BACK:       return "back";
    case KEY_VOLUMEUP:   return "volup";
    case KEY_VOLUMEDOWN: return "voldown";
    case KEY_WAKEUP:     return "wakeup";
    default:             return NULL;
    }
}

/* evdev code -> LVGL key; returns 0 if not representable */
static uint32_t to_lv_key(uint16_t code)
{
    switch (code) {
    case KEY_UP:        return LV_KEY_UP;
    case KEY_DOWN:      return LV_KEY_DOWN;
    case KEY_LEFT:      return LV_KEY_LEFT;
    case KEY_RIGHT:     return LV_KEY_RIGHT;
    case KEY_ENTER:     return LV_KEY_ENTER;
    case KEY_OK:        return LV_KEY_ENTER;
    case KEY_ESC:       return LV_KEY_ESC;
    case KEY_BACKSPACE: return LV_KEY_BACKSPACE;
    case KEY_DELETE:    return LV_KEY_DEL;
    case KEY_TAB:       return LV_KEY_NEXT;
    case KEY_PAGEUP:    return LV_KEY_PREV;
    case KEY_PAGEDOWN:  return LV_KEY_NEXT;
    case KEY_HOME:      return LV_KEY_HOME;
    case KEY_END:       return LV_KEY_END;
    case KEY_SPACE:     return ' ';
    default:
        /* letters a-z (evdev 30..38 = a..l, 44..50 = z..m, 16..25 = q..p) */
        if (code >= KEY_Q && code <= KEY_P) return 'q' + (code - KEY_Q);
        if (code >= KEY_A && code <= KEY_L) return 'a' + (code - KEY_A);
        if (code >= KEY_Z && code <= KEY_M) return 'z' + (code - KEY_Z);
        /* number row 1..0 (evdev 2..11) */
        if (code >= 2 && code <= 11) return '1' + (code - 2); /* 2..11 -> 1..9,0 */
        if (code == KEY_MINUS) return '-';
        return 0;
    }
}

/* ---- LVGL keypad read ---- */

static void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    evq_item_t ev;
    while (pop_event(&ev)) {
        if (is_system_key(ev.code)) {
            if (ev.state == 1) {
                const char *n = system_key_name(ev.code);
                if (n) qzos_bridge_send_key(n);
            }
            continue;
        }
        if (ev.state == 0) { /* release of last key */
            data->key = s_last_key;
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        uint32_t k = to_lv_key(ev.code);
        if (!k) continue; /* unmapped */
        s_last_key = k;
        data->key = k;
        data->state = LV_INDEV_STATE_PRESSED;
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
                /* repeat: only meaningful for nav/scroll keys */
                if (code != KEY_UP && code != KEY_DOWN &&
                    code != KEY_LEFT && code != KEY_RIGHT &&
                    code != KEY_PAGEUP && code != KEY_PAGEDOWN) continue;
                st = 1;
            }
            push_event(code, st);
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
    return 0;
}
