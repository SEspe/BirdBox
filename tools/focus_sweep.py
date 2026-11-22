#!/usr/bin/env python3
"""Focus sweep: score the sharpness of live-stream frames while the lens is turned.

Usage:  python tools/focus_sweep.py http://192.168.10.35 SECONDS OUTDIR

Reads /stream for SECONDS, scores each frame's centre half, and writes
OUTDIR/frames.csv (time, score, mean brightness) and the JPEG of every frame.
The score is the mean squared neighbour-pixel difference (gradient energy): it
rises as edges sharpen and falls as they blur. Turn the lens during the run.
"""
import csv
import os
import sys
import time
import urllib.request

import numpy as np
from PIL import Image


def score(jpeg):
    img = Image.open(__import__('io').BytesIO(jpeg)).convert('L')
    g = np.asarray(img, dtype=np.float32)
    h, w = g.shape
    c = g[h // 4: 3 * h // 4, w // 4: 3 * w // 4]
    dx = np.diff(c, axis=1)
    dy = np.diff(c, axis=0)
    return float((dx * dx).mean() + (dy * dy).mean()), float(c.mean())


def frames(url, seconds):
    """Yield (time, jpeg bytes) from an MJPEG stream for SECONDS."""
    end = time.time() + seconds
    buf = b''
    with urllib.request.urlopen(url + '/stream', timeout=15) as r:
        while time.time() < end:
            chunk = r.read(4096)
            if not chunk:
                break
            buf += chunk
            while True:
                s = buf.find(b'\xff\xd8')
                if s < 0:
                    buf = b''
                    break
                e = buf.find(b'\xff\xd9', s + 2)
                if e < 0:
                    buf = buf[s:]
                    break
                yield time.time(), buf[s:e + 2]
                buf = buf[e + 2:]


def main():
    url, seconds, outdir = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    os.makedirs(outdir, exist_ok=True)
    t0 = time.time()
    rows = []
    for t, jpeg in frames(url, seconds):
        s, m = score(jpeg)
        n = len(rows)
        with open(os.path.join(outdir, f'frame_{n:04d}.jpg'), 'wb') as f:
            f.write(jpeg)
        rows.append((round(t - t0, 2), round(s, 1), round(m, 1)))
        print(f'{rows[-1][0]:7.2f}s  score {s:10.1f}  mean {m:6.1f}', flush=True)
    with open(os.path.join(outdir, 'frames.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['time_s', 'score', 'mean'])
        w.writerows(rows)
    if rows:
        best = max(rows, key=lambda r: r[1])
        print(f'peak: {best[1]} at {best[0]} s over {len(rows)} frames')


if __name__ == '__main__':
    main()
