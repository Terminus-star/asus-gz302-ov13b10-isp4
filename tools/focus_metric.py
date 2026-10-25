#!/usr/bin/env python3
"""Per-frame brightness and sharpness of a captured NV12 file (the .nv12 files of test-isp4-threshold.sh).

usage: python3 -I tools/focus_metric.py FILE.nv12 [--size 1920x1080] [--crop 400] [--step 2]

For every frame it prints the mean luma (Y) of the whole frame and a sharpness score: the mean
absolute 4-neighbour Laplacian |4*Y(x,y) - Y(x-1,y) - Y(x+1,y) - Y(x,y-1) - Y(x,y+1)| over a square
crop of --crop pixels in the frame centre, sampling every --step-th pixel. Higher = more fine detail.
The score also grows with noise and brightness, so only compare runs of the same scene and exposure
(e.g. two lens positions); the last frames are the meaningful ones once auto exposure has settled.
The third column, temporal noise, is the mean |Y(t) - Y(t-1)| at the same sampled pixels: in a STILL
scene with settled exposure only noise changes a pixel between frames, so lower = cleaner image.
Plain Python, no numpy: reads the file, writes nothing.
"""
import argparse
import os
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("file")
    ap.add_argument("--size", default="1920x1080", help="frame size WIDTHxHEIGHT (default 1920x1080)")
    ap.add_argument("--crop", type=int, default=400, help="side of the centre crop in pixels (default 400)")
    ap.add_argument("--step", type=int, default=2, help="sample every STEP-th pixel (default 2)")
    a = ap.parse_args()

    w, h = (int(v) for v in a.size.lower().split("x"))
    frame = w * h * 3 // 2                       # NV12: Y plane w*h, then interleaved UV w*h/2
    total = os.path.getsize(a.file)
    if total < frame:
        sys.exit(f"{a.file}: {total} bytes, smaller than one {w}x{h} NV12 frame ({frame} bytes)")
    n = total // frame
    c = min(a.crop, w - 2, h - 2)
    x0, y0 = (w - c) // 2, (h - c) // 2

    print(f"{a.file}: {n} frames of {w}x{h}; sharpness over a {c}x{c} centre crop, step {a.step}")
    print("frame  meanY  sharpness  temporal-noise")
    prev = None
    with open(a.file, "rb") as f:
        for i in range(n):
            y = f.read(frame)[:w * h]
            mean_y = sum(y[::97]) / len(y[::97])  # sampled: plenty for an average
            acc = 0
            cnt = 0
            samples = []
            for yy in range(y0 + 1, y0 + c - 1, a.step):
                row = yy * w
                for xx in range(x0 + 1, x0 + c - 1, a.step):
                    p = row + xx
                    acc += abs(4 * y[p] - y[p - 1] - y[p + 1] - y[p - w] - y[p + w])
                    cnt += 1
                    samples.append(y[p])
            if prev is None:
                tn = "-"
            else:
                tn = f"{sum(abs(s - q) for s, q in zip(samples, prev)) / cnt:.3f}"
            prev = samples
            print(f"{i:5d}  {mean_y:5.1f}  {acc / cnt:9.3f}  {tn:>14}")


if __name__ == "__main__":
    main()
