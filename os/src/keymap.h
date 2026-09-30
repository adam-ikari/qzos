/*
 * keymap.h — evdev 键码 -> 宿主按键语义（纯函数、零状态、零 I/O、C99）
 *
 * 单独拆出来的理由和显示层一样：键码映射是那种"看起来像显然、错了却只
 * 表现为屏幕上打出别的字母"的代码。这台设备没有触摸，键盘是唯一输入，
 * 而字母键的 evdev 码**不是 ASCII 连续段**（KEY_Q=16、KEY_A=30、KEY_Z=44，
 * 中间跳过一堆修饰键）。早先的版本假设它们连续，用 `'a' + (code - KEY_A)`
 * 一把算过去，于是 KEY_H 打出 'f'、KEY_0 打出 ':'——每个字母都错，且完全
 * 静默。所以映射必须逐项写死，并且能被单测钉住。
 *
 * 不依赖 LVGL：返回宿主自己的 qzos_key_t，由 input.c 翻成 LV_KEY_*。
 * 这样这张表能脱离宿主构建、脱离设备单测（见 scripts/test-keymap.sh）。
 */
#ifndef QZOS_KEYMAP_H
#define QZOS_KEYMAP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    QZ_KEY_NONE = 0,   /* 不映射到任何 LVGL 按键 */
    QZ_KEY_UP,
    QZ_KEY_DOWN,
    QZ_KEY_LEFT,
    QZ_KEY_RIGHT,
    QZ_KEY_ENTER,
    QZ_KEY_ESC,
    QZ_KEY_BACKSPACE,
    QZ_KEY_DEL,
    QZ_KEY_NEXT,       /* Tab / PageDown：group 焦点下一个 */
    QZ_KEY_PREV,       /* PageUp：group 焦点上一个 */
    QZ_KEY_HOME,
    QZ_KEY_END,
    QZ_KEY_CHAR        /* 可打印字符，字符值写入 *out_char */
} qzos_key_t;

/* 解码一个 evdev 键码。返回 QZ_KEY_CHAR 时把字符写进 *out_char。
 * out_char 可为 NULL（只关心是不是字符键）。 */
qzos_key_t qzos_keymap_decode(uint16_t evdev_code, char *out_char);

/* 系统键（Home/Back/音量/唤醒）：不进 LVGL，直接作为 JSON 事件投给 JS shell */
bool qzos_keymap_is_system(uint16_t evdev_code);
const char *qzos_keymap_system_name(uint16_t evdev_code); /* 非系统键返回 NULL */

/* 长按连发（evdev value==2）只对导航键有意义——字母连发会往 textarea 里
 * 灌一串重复字符，所以其余键的 repeat 事件被丢弃。 */
bool qzos_keymap_repeatable(uint16_t evdev_code);

#endif /* QZOS_KEYMAP_H */
