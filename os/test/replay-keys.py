#!/usr/bin/env python3
"""replay-keys: 把一段按键脚本写成 evdev 事件流（写到 stdout / FIFO）。

用法:
    replay-keys --arch x86_64|mips32 --script "down,down,enter,back" \
                [--gap-ms 250] [--hold-ms 40] > keys.bin

布局陷阱：`struct input_event` 的字节大小随架构不同（x86_64 = 24，
mips32 = 16），因为 timeval 里的 time_t 是 8 还是 4 字节。宿主写的字节流
被 MIPS 客体按 16 字节切分就会全乱 —— 所以必须显式声明消费者的架构，
不能靠"在同一个机器上跑"蒙混过关。
"""
import argparse
import struct
import sys
import time

# Linux input-event codes actually routed by os/src/input.c.
#
# 字母/数字**不能**用 ASCII 当 code：evdev 的 KEY_* 是历史遗留的错位表
# （KEY_A=30、KEY_H=35、KEY_Z=44…），而且这些码和导航键的码段重叠——
# 用 ASCII 会把 'h'(104) 变成 KEY_PAGEUP、'i'(105) 变成 KEY_LEFT，脚本
# 于是"成功"地按下另一个键，测试通过而什么都没测到。
# 同一张表见 C1Terminal keyboard.go / os/src/input.c to_lv_key()。
CODES = {
    "up": 103, "down": 108, "left": 105, "right": 106,
    "enter": 28, "ok": 352, "esc": 1, "backspace": 14, "del": 111,
    "home": 102, "back": 158, "space": 57, "tab": 15,
    "pageup": 104, "pagedown": 109,
    # 电源/睡眠键。键值取自内核 <linux/input.h>：KEY_POWER=116、KEY_SLEEP=142。
    # 设备实际用哪个码仍属未验（brain qzos-power-sim 的 P1），所以两个都给，
    # 让「换了码也不影响测试结构」这件事保持成立。
    "power": 116, "sleep": 142,
    "1": 2, "2": 3, "3": 4, "4": 5, "5": 6,
    "6": 7, "7": 8, "8": 9, "9": 10, "0": 11,
    "q": 16, "w": 17, "e": 18, "r": 19, "t": 20, "y": 21, "u": 22,
    "i": 23, "o": 24, "p": 25,
    "a": 30, "s": 31, "d": 32, "f": 33, "g": 34, "h": 35, "j": 36,
    "k": 37, "l": 38,
    "z": 44, "x": 45, "c": 46, "v": 47, "b": 48, "n": 49, "m": 50,
}
# 同一个 code 不能有两种叫法：那会让脚本"合法"地发出一个和意图不同的键。
assert len(set(CODES.values())) == len(CODES), "duplicate evdev code in CODES"
# `struct timeval {i64/i32 secs; i64/i32 usec}` + u16 type + u16 code + s32 value
LAYOUTS = {"x86_64": "qqHHi", "mips32": "iiHHi"}


def ev(arch, etype, code, value):
    return struct.pack(LAYOUTS[arch], 0, 0, etype, code, value)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", choices=sorted(LAYOUTS), required=True)
    ap.add_argument("--script", required=True,
                    help="逗号分隔的键名，如 down,down,enter")
    ap.add_argument("--gap-ms", type=int, default=250, help="每击之间的间隔")
    ap.add_argument("--hold-ms", type=int, default=40, help="按下到抬起的间隔")
    ap.add_argument("--out", help="输出文件（缺省 stdout）")
    a = ap.parse_args()

    out = open(a.out, "wb") if a.out else sys.stdout.buffer
    for i, tok in enumerate(t.strip() for t in a.script.split(",")):
        if not tok:
            continue
        key = tok.lower()
        if key not in CODES:
            sys.exit(f"unknown key {tok!r}; known: {' '.join(sorted(CODES))}")
        code = CODES[key]
        if i:
            time.sleep(a.gap_ms / 1000.0)
        out.write(ev(a.arch, 0x01, code, 1))          # EV_KEY press
        time.sleep(a.hold_ms / 1000.0)
        out.write(ev(a.arch, 0x01, code, 0))          # EV_KEY release
        out.flush()
    out.close()


if __name__ == "__main__":
    main()
