#!/usr/bin/env bash
# scripts/frame-preview.sh — 目检 qzos 帧输出
#
# 1bpp e-ink 没有截图服务（设备是裸机 rootfs，无 Android/screencap），
# 验证走导出帧 + ASCII 预览这条通道：
#   QZ_DISPLAY=pbm  QZ_PBM=/tmp/f.pbm   阈值化后的 1bpp 帧（P4）
#   QZ_DUMP_RAW=... QZ_DUMP_RAW=/tmp/f.pgm  L8 灰度中间帧（P5，看阈值前）
#
# 用法:
#   scripts/frame-preview.sh /tmp/qzos-frame.pbm      # 1bpp 帧（默认）
#   scripts/frame-preview.sh /tmp/qzos-raw.pgm        # L8 灰度帧
set -euo pipefail
F="${1:-/tmp/qzos-frame.pbm}"
[ -f "$F" ] || { echo "no frame at $F" >&2; exit 1; }

python3 - "$F" <<'PY'
import signal, sys
# 被 head/less 截断时安静退出：BrokenPipeError 打到 stderr 会盖掉真正的输出
signal.signal(signal.SIGPIPE, signal.SIG_DFL)
path = sys.argv[1]
data = open(path, 'rb').read()
magic = data.split(None, 1)[0]

if magic == b'P4':          # 1bpp：bit=黑
    parts = data.split(None, 3)
    w, h = int(parts[1]), int(parts[2])
    px = parts[3]
    rb = (w + 7) // 8
    px_at = lambda x, y: (px[y * rb + x // 8] >> (7 - (x % 8))) & 1
    dark = sum(bin(b).count('1') for b in px)
    print(f'{path}: P4 {w}x{h}  dark bits={dark} ({100.0*dark/(w*h):.1f}% ink)')
    ramp = {0: '.', 1: '#'}
elif magic == b'P5':        # L8：0=黑 255=白
    parts = data.split(None, 4)
    w, h, maxv = int(parts[1]), int(parts[2]), int(parts[3])
    px = parts[4]
    px_at = lambda x, y: 1 if px[y * w + x] < 128 else 0
    hist = {}
    for v in px:
        hist[v] = hist.get(v, 0) + 1
    print(f'{path}: P5 {w}x{h} max={maxv}  '
          f'top={sorted(hist.items(), key=lambda kv: -kv[1])[:4]}')
    ramp = None
else:
    sys.exit(f'unknown PGM/PPM magic {magic!r}')

for y in range(0, h, 2):
    print('%3d %s' % (y, ''.join(ramp[px_at(x, y)] if ramp else
                                 ('.' if px[y*w+x] >= 200 else 'o' if px[y*w+x] >= 100 else '#')
                                 for x in range(0, w, 2))))
PY
