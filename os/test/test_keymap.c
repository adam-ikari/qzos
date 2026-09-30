/*
 * test_keymap.c — 键码映射单测（纯逻辑，无 LVGL / 无设备 / 无宿主构建）
 *
 * 测试怎么才不会被自己骗：**键码常量全部取自内核头 <linux/input.h>**，
 * 只有期望的字符是字面量。所以断言问的是"内核说 KEY_H 是这个码，我们的表
 * 必须把它变成 'h'"，而不是"我们表里的 35 是不是 35"。
 * 这正是当初漏掉的那个 bug 的形状：映射表用算术从码推字符，于是
 * KEY_H(35) 推出 'f'，而如果测试两侧都用同一张手抄表，就永远测不出来。
 */
#include <linux/input.h>
#include <stdio.h>
#include <string.h>

#include "keymap.h"

static int checks, failed;

static void expect_char(uint16_t code, char want, const char *name)
{
    char got = 0;
    qzos_key_t k = qzos_keymap_decode(code, &got);
    checks++;
    if (k != QZ_KEY_CHAR || got != want) {
        failed++;
        printf("FAIL %-8s code=%u -> {%s, '%c'}, want {CHAR, '%c'}\n",
               name, code,
               k == QZ_KEY_CHAR ? "CHAR" : (k == QZ_KEY_NONE ? "NONE" : "NAV"),
               got ? got : '?', want);
    }
}

static void expect_key(uint16_t code, qzos_key_t want, const char *name)
{
    qzos_key_t k = qzos_keymap_decode(code, NULL);
    checks++;
    if (k != want) {
        failed++;
        printf("FAIL %-10s code=%u -> %d, want %d\n", name, code, (int)k, (int)want);
    }
}

static void expect_none(uint16_t code, const char *name)
{
    expect_key(code, QZ_KEY_NONE, name);
}

int main(void)
{
    /* ---- 字母：逐项写死的意义就在这里 ---- */
    expect_char(KEY_Q, 'q', "q"); expect_char(KEY_W, 'w', "w");
    expect_char(KEY_E, 'e', "e"); expect_char(KEY_R, 'r', "r");
    expect_char(KEY_T, 't', "t"); expect_char(KEY_Y, 'y', "y");
    expect_char(KEY_U, 'u', "u"); expect_char(KEY_I, 'i', "i");
    expect_char(KEY_O, 'o', "o"); expect_char(KEY_P, 'p', "p");
    expect_char(KEY_A, 'a', "a"); expect_char(KEY_S, 's', "s");
    expect_char(KEY_D, 'd', "d"); expect_char(KEY_F, 'f', "f");
    expect_char(KEY_G, 'g', "g"); expect_char(KEY_H, 'h', "h");
    expect_char(KEY_J, 'j', "j"); expect_char(KEY_K, 'k', "k");
    expect_char(KEY_L, 'l', "l");
    expect_char(KEY_Z, 'z', "z"); expect_char(KEY_X, 'x', "x");
    expect_char(KEY_C, 'c', "c"); expect_char(KEY_V, 'v', "v");
    expect_char(KEY_B, 'b', "b"); expect_char(KEY_N, 'n', "n");
    expect_char(KEY_M, 'm', "m");

    /* ---- 数字与符号 ---- */
    expect_char(KEY_1, '1', "1"); expect_char(KEY_2, '2', "2");
    expect_char(KEY_3, '3', "3"); expect_char(KEY_4, '4', "4");
    expect_char(KEY_5, '5', "5"); expect_char(KEY_6, '6', "6");
    expect_char(KEY_7, '7', "7"); expect_char(KEY_8, '8', "8");
    expect_char(KEY_9, '9', "9"); expect_char(KEY_0, '0', "0");
    expect_char(KEY_MINUS, '-', "-"); expect_char(KEY_EQUAL, '=', "=");
    expect_char(KEY_SPACE, ' ', "space");

    /* 26 个字母 + 10 个数字必须两两不同：漏写一项会让两个键打出同一个
     * 字符，而逐项断言未必发现（例如把 KEY_O 打成 0 附近的符号）。 */
    {
        static const uint16_t codes[] = {
            KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P,
            KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
            KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M,
            KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
        };
        int n = (int)(sizeof codes / sizeof codes[0]);
        checks++;
        for (int i = 0; i < n; i++) {
            char a = 0, b = 0;
            qzos_keymap_decode(codes[i], &a);
            for (int j = i + 1; j < n; j++) {
                qzos_keymap_decode(codes[j], &b);
                if (a == b) {
                    failed++;
                    printf("FAIL codes %u and %u both produce '%c'\n",
                           codes[i], codes[j], a);
                    break;
                }
            }
        }
    }

    /* ---- 导航/编辑键 ---- */
    expect_key(KEY_UP, QZ_KEY_UP, "up");
    expect_key(KEY_DOWN, QZ_KEY_DOWN, "down");
    expect_key(KEY_LEFT, QZ_KEY_LEFT, "left");
    expect_key(KEY_RIGHT, QZ_KEY_RIGHT, "right");
    expect_key(KEY_ENTER, QZ_KEY_ENTER, "enter");
    expect_key(KEY_OK, QZ_KEY_ENTER, "ok=enter");
    expect_key(KEY_ESC, QZ_KEY_ESC, "esc");
    expect_key(KEY_BACKSPACE, QZ_KEY_BACKSPACE, "backspace");
    expect_key(KEY_DELETE, QZ_KEY_DEL, "delete");
    expect_key(KEY_TAB, QZ_KEY_NEXT, "tab=next");
    expect_key(KEY_PAGEUP, QZ_KEY_PREV, "pgup=prev");
    expect_key(KEY_PAGEDOWN, QZ_KEY_NEXT, "pgdn=next");
    expect_key(KEY_END, QZ_KEY_END, "end");

    /* 未映射的修饰键必须落到 NONE，而不是随便找个键——误映射会让
     * Ctrl/Shift 之类在文本框里打出垃圾字符。 */
    expect_none(KEY_LEFTCTRL, "lctrl");
    expect_none(KEY_LEFTALT, "lalt");
    expect_none(KEY_LEFTSHIFT, "lshift");
    expect_none(KEY_CAPSLOCK, "capslk");
    expect_none(0, "code0");

    /* ---- 系统键：不进 LVGL，直接投给 JS shell ---- */
    {
        static const struct { uint16_t code; const char *name; } sys[] = {
            { KEY_HOME, "home" }, { KEY_BACK, "back" },
            { KEY_VOLUMEUP, "volup" }, { KEY_VOLUMEDOWN, "voldown" },
            { KEY_WAKEUP, "wakeup" },
        };
        for (size_t i = 0; i < sizeof sys / sizeof sys[0]; i++) {
            checks++;
            if (!qzos_keymap_is_system(sys[i].code)) {
                failed++;
                printf("FAIL is_system(%u) false\n", sys[i].code);
            }
            checks++;
            const char *n = qzos_keymap_system_name(sys[i].code);
            if (!n || strcmp(n, sys[i].name) != 0) {
                failed++;
                printf("FAIL system_name(%u) = %s, want %s\n",
                       sys[i].code, n ? n : "(null)", sys[i].name);
            }
        }
        /* 普通键不能被误判成系统键 */
        checks++;
        if (qzos_keymap_is_system(KEY_A) || qzos_keymap_is_system(KEY_ENTER) ||
            qzos_keymap_is_system(KEY_UP) || qzos_keymap_system_name(KEY_A)) {
            failed++;
            printf("FAIL ordinary key reported as system key\n");
        }
    }

    /* ---- 长按连发：只对导航键开放 ---- */
    {
        static const uint16_t nav[] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
                                        KEY_PAGEUP, KEY_PAGEDOWN };
        for (size_t i = 0; i < sizeof nav / sizeof nav[0]; i++) {
            checks++;
            if (!qzos_keymap_repeatable(nav[i])) {
                failed++;
                printf("FAIL repeatable(%u) false\n", nav[i]);
            }
        }
        /* 字母连发会往 textarea 灌重复字符，必须丢弃 */
        static const uint16_t norpt[] = { KEY_A, KEY_H, KEY_1, KEY_SPACE,
                                          KEY_ENTER, KEY_BACK, KEY_HOME };
        for (size_t i = 0; i < sizeof norpt / sizeof norpt[0]; i++) {
            checks++;
            if (qzos_keymap_repeatable(norpt[i])) {
                failed++;
                printf("FAIL repeatable(%u) true (char/system key must not repeat)\n",
                       norpt[i]);
            }
        }
    }

    printf("%s: %d checks, %d failed\n", failed ? "FAIL" : "OK", checks, failed);
    return failed ? 1 : 0;
}
