#!/usr/bin/env python3
"""Write the benchmark's raw-frame fixtures (deterministic, standard library only).

  make-fixtures.py [OUTDIR]     default: this directory

The frames are synthetic: no capture, no personal content. They exercise the
`DecoderTest --bgr24-bench --input` path with a padded stride and both payload
conventions. `--check` regenerates into a temporary directory and fails when the
committed files differ, so the fixtures can't drift from this script.
"""
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent


def frame(width, height, stride, padded):
    """BGR24, top-down. A smooth scene-like image: horizontal and vertical ramps,
    a diagonal band and a corner marker, so neighbouring pixels are similar like
    real video (unlike the benchmark's random pattern). Padding bytes are 0xEE."""
    out = bytearray()
    for y in range(height):
        row = bytearray()
        for x in range(width):
            b = (x * 255) // (width - 1)
            g = (y * 255) // (height - 1)
            r = 200 if abs(x - y * width // height) < 4 else 40
            if x < 4 and y < 4:          # top-left marker: white, orientation check
                b = g = r = 255
            row += bytes((b, g, r))
        last = y == height - 1
        if padded or not last:
            row += b"\xee" * (stride - width * 3)
        out += row
    return bytes(out)


FIXTURES = [
    # name, width, height, stride, padded last row, declared range
    ("scene-64x36-padded", 64, 36, 200, True, "full"),
    ("scene-64x36-tight", 64, 36, 192, False, "limited"),
]


def write(outdir):
    outdir = pathlib.Path(outdir)
    for name, w, h, stride, padded, rng in FIXTURES:
        data = frame(w, h, stride, padded)
        (outdir / (name + ".bgr24")).write_bytes(data)
        (outdir / (name + ".desc")).write_text(
            "# hyperhdr raw frame description, version 1\n"
            "format = BGR24\n"
            "width = %d\nheight = %d\nstride = %d\n"
            "range = %s\n"
            "orientation = top-down\n"
            "payload = %d\n"
            "source = synthetic scene (tests/bench_report/fixtures/make-fixtures.py), no personal content\n"
            % (w, h, stride, rng, len(data)))


if __name__ == "__main__":
    if sys.argv[1:] == ["--check"]:
        with tempfile.TemporaryDirectory() as tmp:
            write(tmp)
            bad = [p.name for p in pathlib.Path(tmp).iterdir() if (HERE / p.name).read_bytes() != p.read_bytes()]
        if bad:
            print("fixtures differ from make-fixtures.py: " + ", ".join(sorted(bad)))
            sys.exit(1)
        print("fixtures match make-fixtures.py")
    else:
        write(sys.argv[1] if len(sys.argv) > 1 else HERE)
