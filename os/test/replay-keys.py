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

# Linux input-event codes actually routed by os/src/input.c
CODES = {
    "up": 103, "down": 108, "left": 105, "right": 106,
    "enter": 28, "ok": 352, "esc": 1, "backspace": 14, "del": 111,
    "home": 102, "back": 158, "space": 57, "tab": 15,
    "pageup": 104, "pagedown": 109,
    **{chr(c): c for c in range(ord("a"), ord("z") + 1)},
    **{chr(c): c for c in range(ord("0"), ord("9") + 1)},
}
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
