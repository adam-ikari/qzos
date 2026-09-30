#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 LVGL 像素字体用的字符集 symbols.txt（GB2312 一级汉字 + ASCII + 常用符号）。"""
import os

chars = []
# GB2312 一级汉字（第 16-55 区）
for row in range(16, 56):
    for col in range(0xA1, 0xFF):
        b = bytes([0xA0 + row, col])
        try:
            chars.append(b.decode("gb2312"))
        except Exception:
            pass

extra = [chr(c) for c in range(0x20, 0x7F)]              # ASCII 可打印
extra += [chr(c) for c in range(0x3000, 0x3040)]          # CJK 标点
extra += [chr(c) for c in range(0xFF01, 0xFF5F)]          # 全角形式
extra += [chr(c) for c in range(0x2010, 0x201F)]          # 各种破折号/引号
extra += ["\u2026", "\u00b7"]                             # … ·
extra += [chr(c) for c in range(0x2190, 0x2194)]          # ← ↑ → ↓
extra += [chr(c) for c in range(0x2500, 0x2580)]          # 制表符/方块
extra += [chr(c) for c in range(0x25A0, 0x25FF)]          # 几何图形

seen, out = set(), []
for ch in extra + chars:
    if ch not in seen and ch.strip() != "":
        seen.add(ch)
        out.append(ch)

s = "".join(out)
os.makedirs(os.path.dirname(os.path.abspath(__file__)), exist_ok=True)
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "symbols.txt"),
          "w", encoding="utf-8") as f:
    f.write(s)
print("glyphs:", len(s))
