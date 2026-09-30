#!/usr/bin/env python3
"""pbm_view.py — 把 qzos 的 1bpp 帧渲染成可目检的 PNG

为什么需要它：`/dev/epaper_lcd` 只写不可读，屏上一切只能靠帧文件判断。
而「墨量多少」这种阈值判据看不见内容——坏包启动后画面多了 600 个墨点，
和某个控件重排导致的墨量变化在数值上区分不开。于是判据退化成装饰：
一个「看起来在测、其实什么都没测」的阈值。

真正能判别的是**看清画面上写了什么**。本脚本把 296x152 1bpp 帧按最近邻
放大成 PNG，供人眼与多模态识别核对；另提供 `--region` 子命令做区域级
判定，让「某个标记没出现在画面上」这类断言可以自动化。

用法：
  pbm_view.py <in.pbm> [out.png] [--scale N] [--invert]
      渲染 PNG。1bpp 放大必须最近邻——任何插值都会把 1px 笔画糊成灰边。

  pbm_view.py --region <in.pbm> --at X,Y,W,H [--at ...] [--max N]
      数指定矩形里的墨点数，判据落在「这一块**没有**东西」上。
      可给多个 --at；任一区域超 --max 即退出码 1。

极性：本面板**置位=黑**（brain: mask=0x80>>(y%8)，置位为黑），所以
PBM 里的 1 是黑像素。--invert 供前景/背景相反的面板使用。
"""
import sys
import argparse


def read_pbm(path):
    """读 P4 (PBM, 1bpp) 帧。

    P4 的每行按字节向上取整填充，所以必须**逐行**切，不能把整个文件当一条
    位流（那是 P5/裸位图的做法）——本屏 296 不是 8 的倍数，整文件当位流会
    逐行错位，渲出来是斜的。
    """
    data = open(path, "rb").read()
    fields, i = [], 2
    while len(fields) < 2:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":                     # 注释行
            while i < len(data) and data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1                                          # 跳过头后的一个空白
    w, h = fields[0], fields[1]
    stride = (w + 7) // 8
    rows = []
    for y in range(h):
        row = data[i + y * stride: i + (y + 1) * stride]
        if len(row) < stride:
            row = row + b"\x00" * (stride - len(row))
        rows.append(row)
    return w, h, rows


def px(rows, w, x, y, invert=False):
    bit = (rows[y][x >> 3] >> (7 - (x & 7))) & 1
    return (1 - bit) if invert else bit              # True = 有墨


def to_png(w, h, rows, scale=3, invert=False):
    from PIL import Image
    # PIL "1" 模式：0=黑、255=白
    img = Image.new("1", (w, h), 255)
    p = img.load()
    for y in range(h):
        for x in range(w):
            p[x, y] = 255 if px(rows, w, x, y, invert) else 0
    if scale > 1:
        img = img.resize((w * scale, h * scale), Image.NEAREST)
    return img


def region_ink(w, h, rows, rect, invert=False):
    x, y, rw, rh = rect
    n = 0
    for yy in range(max(0, y), min(h, y + rh)):
        for xx in range(max(0, x), min(w, x + rw)):
            if px(rows, w, xx, yy, invert):
                n += 1
    return n


def parse_rect(s):
    parts = s.replace(" ", "").split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("--at 需要 X,Y,W,H")
    return tuple(int(p) for p in parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("infile", nargs="?")
    ap.add_argument("outfile", nargs="?", default=None)
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--invert", action="store_true")
    ap.add_argument("--region", action="store_true", help="区域墨量模式")
    ap.add_argument("--at", type=parse_rect, action="append", default=[],
                    metavar="X,Y,W,H")
    ap.add_argument("--max", type=int, default=-1,
                    help="任一区域超过此值即失败（缺省 -1 = 只测量不判定）")
    a = ap.parse_args()

    if not a.infile:
        ap.error("需要帧文件")

    w, h, rows = read_pbm(a.infile)

    if a.region:
        if not a.at:
            ap.error("--region 需要至少一个 --at")
        worst = 0
        for r in a.at:
            n = region_ink(w, h, rows, r, a.invert)
            worst = max(worst, n)
            print("%s at %s -> %d ink px" % (a.infile, r, n))
        # --max 缺省为 -1 = 只测量不判定。默认 0 会在「量出任何墨量」时判失败，
        # 而调用方往往只是想**读**这个数字（sed 取出来自己比阈值）。
        if a.max >= 0:
            if worst > a.max:
                print("FAIL: 区域墨量 %d > %d（标记出现在画面上？）" % (worst, a.max))
                return 1
            print("OK: 区域墨量均 <= %d" % a.max)
        return 0

    img = to_png(w, h, rows, a.scale, a.invert)
    out = a.outfile or (a.infile.rsplit(".", 1)[0] + ".png")
    img.save(out)
    total = sum(region_ink(w, h, rows, (0, 0, w, h), a.invert) for _ in [0])
    print("%s -> %s  (%dx%d, scale %d, %d ink px%s)"
          % (a.infile, out, w, h, a.scale, total,
             ", inverted" if a.invert else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
