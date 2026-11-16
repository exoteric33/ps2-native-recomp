#!/usr/bin/env python3
"""Compare two pictures of the same frame: the painter's and the card path's.

  python3 scripts/hwdiff.py PAINTER.ppm CARD.ppm
  python3 scripts/hwdiff.py --prefix shot                      # shot-compute.ppm against shot-hw.ppm (what PS2_GPU_BENCH_SHOT writes)
  python3 scripts/hwdiff.py --pair all a-compute.ppm a-hw.ppm --pair glow b-compute.ppm b-hw.ppm   # one row per pair, e.g. per class of PS2_GPU_HW_ONLY
  python3 scripts/hwdiff.py A.ppm B.ppm --crop 100,80,64,64 --out crop    # crop-painter.png, crop-card.png, crop-diff4.png (difference times 4)
  python3 scripts/hwdiff.py --self-test

Per pair it prints the share of pixels that differ at all, by more than 8 and by more than 32 (of 255, in any channel), the mean absolute
difference per channel, the largest difference, and whether the numbers meet the proposed whole-frame tolerance
(at most 2 % of pixels off by more than 8, mean difference at most 1.5 per channel). The tolerance is a flag in the output, not an exit
code, unless --strict is given (exit 1 when a pair is outside it). Pictures are binary PPM (P6) or PNG; sizes must be equal.
Needs numpy; PNG files and crops need Pillow (PPM crops are written as PPM without it).
"""
import argparse
import json
import sys
from pathlib import Path

try:
    import numpy as np
except ImportError:  # the numbers are the point of this script
    sys.exit("hwdiff: numpy is needed (pip install numpy)")


def read_picture(path):
    """Return an (h, w, 3) uint8 array."""
    data = Path(path).read_bytes()
    if data[:2] == b"P6":
        pos, fields = 2, []
        while len(fields) < 3:
            while data[pos:pos + 1].isspace():
                pos += 1
            if data[pos:pos + 1] == b"#":
                while data[pos:pos + 1] not in (b"\n", b""):
                    pos += 1
                continue
            start = pos
            while not data[pos:pos + 1].isspace():
                pos += 1
            fields.append(int(data[start:pos]))
        pos += 1  # one whitespace byte after maxval
        w, h, maxval = fields
        if maxval != 255:
            sys.exit(f"hwdiff: {path}: maxval {maxval} (only 255 is supported)")
        body = np.frombuffer(data, dtype=np.uint8, count=w * h * 3, offset=pos)
        return body.reshape(h, w, 3)
    try:
        from PIL import Image
    except ImportError:
        sys.exit(f"hwdiff: {path} is not a binary PPM and Pillow is not installed")
    return np.asarray(Image.open(path).convert("RGB"))


def write_picture(path, pic):
    path = Path(path)
    try:
        from PIL import Image
        Image.fromarray(pic).save(path.with_suffix(".png"))
        return path.with_suffix(".png")
    except ImportError:
        h, w, _ = pic.shape
        path = path.with_suffix(".ppm")
        path.write_bytes(b"P6\n%d %d\n255\n" % (w, h) + pic.tobytes())
        return path


def compare(a, b):
    if a.shape != b.shape:
        sys.exit(f"hwdiff: sizes differ: {a.shape[1]}x{a.shape[0]} against {b.shape[1]}x{b.shape[0]}")
    d = np.abs(a.astype(np.int16) - b.astype(np.int16))
    worst = d.max(axis=2)  # per pixel: the largest channel difference
    n = worst.size
    return {
        "pixels": int(n),
        "differ_pct": 100.0 * np.count_nonzero(worst) / n,
        "over8_pct": 100.0 * np.count_nonzero(worst > 8) / n,
        "over32_pct": 100.0 * np.count_nonzero(worst > 32) / n,
        "mean_per_channel": [float(x) for x in d.reshape(-1, 3).mean(axis=0)],
        "mean": float(d.mean()),
        "largest": int(d.max()),
    }


def tolerance_ok(r):
    return r["over8_pct"] <= 2.0 and r["mean"] <= 1.5


def row(label, r):
    m = r["mean_per_channel"]
    return (f"{label:<18} {r['differ_pct']:6.2f} % differ  {r['over8_pct']:6.2f} % >8  {r['over32_pct']:6.2f} % >32  "
            f"mean {r['mean']:5.2f} (R {m[0]:.2f} G {m[1]:.2f} B {m[2]:.2f})  largest {r['largest']:3d}  "
            f"{'within' if tolerance_ok(r) else 'OUTSIDE'} the 2.3 whole-frame tolerance")


def crop_files(a, b, spec, out):
    x, y, w, h = (int(v) for v in spec.split(","))
    ca, cb = a[y:y + h, x:x + w], b[y:y + h, x:x + w]
    diff = np.clip(np.abs(ca.astype(np.int16) - cb.astype(np.int16)) * 4, 0, 255).astype(np.uint8)
    return [write_picture(f"{out}-painter", ca), write_picture(f"{out}-card", cb), write_picture(f"{out}-diff4", diff)]


def self_test():
    rng = np.random.default_rng(1)
    a = rng.integers(0, 256, (32, 48, 3), dtype=np.uint8)
    same = compare(a, a)
    assert same["differ_pct"] == 0 and same["largest"] == 0 and tolerance_ok(same)
    b = a.copy()
    b[0, :, 0] = np.clip(b[0, :, 0].astype(np.int16) + 40, 0, 255)  # one row, red channel, +40 (or less where clipped)
    r = compare(a, b)
    assert 0 < r["over8_pct"] <= 100.0 / 32 + 1e-9 and r["largest"] <= 40 and r["largest"] > 8, r
    c = a.copy()
    c[:, :, 1] = np.clip(c[:, :, 1].astype(np.int16) + 1, 0, 255)  # every pixel off by one level in green: all differ, none over 8
    r = compare(a, c)
    assert r["over8_pct"] == 0 and r["differ_pct"] > 99 and abs(r["mean_per_channel"][1] - 1) < 0.01 and r["mean_per_channel"][0] == 0, r
    tmp = Path("/tmp") / "hwdiff_selftest.ppm"
    tmp.write_bytes(b"P6\n# comment\n48 32\n255\n" + a.tobytes())
    assert (read_picture(tmp) == a).all()
    tmp.unlink()
    print("hwdiff self-test: ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pictures", nargs="*", help="painter picture, card picture")
    ap.add_argument("--prefix", help="compare PREFIX-compute.ppm with PREFIX-hw.ppm")
    ap.add_argument("--pair", nargs=3, action="append", metavar=("LABEL", "PAINTER", "CARD"), help="one more pair, with a label (repeatable)")
    ap.add_argument("--crop", help="x,y,w,h of a crop to write (first pair only)")
    ap.add_argument("--out", default="hwdiff-crop", help="crop file prefix")
    ap.add_argument("--json", action="store_true", help="print the numbers as JSON lines")
    ap.add_argument("--strict", action="store_true", help="exit 1 when a pair is outside the tolerance")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    pairs = []
    if len(args.pictures) == 2:
        pairs.append(("picture", *args.pictures))
    elif args.pictures:
        ap.error("give exactly two pictures (painter, card)")
    if args.prefix:
        pairs.append((args.prefix, f"{args.prefix}-compute.ppm", f"{args.prefix}-hw.ppm"))
    pairs += [tuple(p) for p in args.pair or []]
    if not pairs:
        ap.error("nothing to compare")
    bad = 0
    for i, (label, pa, pb) in enumerate(pairs):
        a, b = read_picture(pa), read_picture(pb)
        r = compare(a, b)
        bad += not tolerance_ok(r)
        print(json.dumps({"label": label, **r}) if args.json else row(label, r))
        if i == 0 and args.crop:
            for f in crop_files(a, b, args.crop, args.out):
                print(f"wrote {f}")
    sys.exit(1 if args.strict and bad else 0)


if __name__ == "__main__":
    main()
