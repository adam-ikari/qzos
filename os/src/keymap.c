/*
 * keymap.c — 见 keymap.h。键码常量取自内核 UAPI 头 <linux/input.h>，
 * 字符映射逐项写死（**不要**用 `'a' + (code - KEY_A)` 这类算术：字母段的
 * evdev 码不连续，那样的写法会静默打出错误的字母）。
 */
#include "keymap.h"

#include <linux/input.h>
#include <stddef.h> /* NULL */

qzos_key_t qzos_keymap_decode(uint16_t code, char *out_char)
{
    /* 导航/编辑键：语义与字符无关，直接一一对应。KEY_OK 是设备面板上的
     * 确认键，与 Enter 同义。 */
    switch (code) {
    case KEY_UP:        return QZ_KEY_UP;
    case KEY_DOWN:      return QZ_KEY_DOWN;
    case KEY_LEFT:      return QZ_KEY_LEFT;
    case KEY_RIGHT:     return QZ_KEY_RIGHT;
    case KEY_ENTER:     return QZ_KEY_ENTER;
    case KEY_OK:        return QZ_KEY_ENTER;
    case KEY_ESC:       return QZ_KEY_ESC;
    case KEY_BACKSPACE: return QZ_KEY_BACKSPACE;
    case KEY_DELETE:    return QZ_KEY_DEL;
    case KEY_TAB:       return QZ_KEY_NEXT;
    case KEY_PAGEUP:    return QZ_KEY_PREV;
    case KEY_PAGEDOWN:  return QZ_KEY_NEXT;
    case KEY_HOME:      return QZ_KEY_HOME;
    case KEY_END:       return QZ_KEY_END;
    default: break;
    }

    /* 可打印字符：字母段与数字段逐项写死。 */
    char c = 0;
    switch (code) {
    case KEY_1: c = '1'; break;
    case KEY_2: c = '2'; break;
    case KEY_3: c = '3'; break;
    case KEY_4: c = '4'; break;
    case KEY_5: c = '5'; break;
    case KEY_6: c = '6'; break;
    case KEY_7: c = '7'; break;
    case KEY_8: c = '8'; break;
    case KEY_9: c = '9'; break;
    case KEY_0: c = '0'; break;
    case KEY_MINUS: c = '-'; break;
    case KEY_EQUAL: c = '='; break;
    case KEY_SPACE: c = ' '; break;

    case KEY_Q: c = 'q'; break;
    case KEY_W: c = 'w'; break;
    case KEY_E: c = 'e'; break;
    case KEY_R: c = 'r'; break;
    case KEY_T: c = 't'; break;
    case KEY_Y: c = 'y'; break;
    case KEY_U: c = 'u'; break;
    case KEY_I: c = 'i'; break;
    case KEY_O: c = 'o'; break;
    case KEY_P: c = 'p'; break;

    case KEY_A: c = 'a'; break;
    case KEY_S: c = 's'; break;
    case KEY_D: c = 'd'; break;
    case KEY_F: c = 'f'; break;
    case KEY_G: c = 'g'; break;
    case KEY_H: c = 'h'; break;
    case KEY_J: c = 'j'; break;
    case KEY_K: c = 'k'; break;
    case KEY_L: c = 'l'; break;

    case KEY_Z: c = 'z'; break;
    case KEY_X: c = 'x'; break;
    case KEY_C: c = 'c'; break;
    case KEY_V: c = 'v'; break;
    case KEY_B: c = 'b'; break;
    case KEY_N: c = 'n'; break;
    case KEY_M: c = 'm'; break;

    default: return QZ_KEY_NONE;
    }
    if (out_char) *out_char = c;
    return QZ_KEY_CHAR;
}

bool qzos_keymap_is_system(uint16_t code)
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

const char *qzos_keymap_system_name(uint16_t code)
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

bool qzos_keymap_repeatable(uint16_t code)
{
    switch (code) {
    case KEY_UP:
    case KEY_DOWN:
    case KEY_LEFT:
    case KEY_RIGHT:
    case KEY_PAGEUP:
    case KEY_PAGEDOWN:
        return true;
    default:
        return false;
    }
}
