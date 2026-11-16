#!/usr/bin/env python3
"""Tests for scripts/rk3588/color-patterns.py (checklist C1/C2/C5 patterns).

Checks that generation is deterministic and lossless, that every manifest region
holds exactly the stated pixel values, that the range models match independent
formulas, and that `check` identifies a correct decode, both wrong range
interpretations, swapped red/blue, mirrored/flipped/rotated pictures, a scaled
preview and small noise, using pictures transformed here.
"""
import hashlib
import importlib.util
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/rk3588/color-patterns.py"
SAMPLES = ROOT / "docs/color-patterns"
spec = importlib.util.spec_from_file_location("color_patterns", SCRIPT)
cp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cp)
failures = 0


def check(cond, msg):
    global failures
    print(("PASS " if cond else "FAIL ") + msg)
    failures += 0 if cond else 1


def run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, text=True)


def encode(w, h, rows, rgba=False, filters=(0,)):
    """Independent PNG writer with every filter type, to exercise the reader."""
    bpp = 4 if rgba else 3
    out, prev = b"", bytes(w * bpp)
    for y, row in enumerate(rows):
        line = row if not rgba else b"".join(row[i:i + 3] + b"\xff" for i in range(0, len(row), 3))
        f = filters[y % len(filters)]
        enc = bytearray()
        for i, v in enumerate(line):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 0:
                p = 0
            elif f == 1:
                p = a
            elif f == 2:
                p = b
            elif f == 3:
                p = (a + b) // 2
            else:
                q = a + b - c
                pa, pb, pc = abs(q - a), abs(q - b), abs(q - c)
                p = a if pa <= pb and pa <= pc else b if pb <= pc else c
            enc.append((v - p) & 255)
        out += bytes([f]) + bytes(enc)
        prev = line

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6 if rgba else 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(out)) + chunk(b"IEND", b""))


# --- range models against independent formulas ---------------------------------------
check(all(cp.not_expanded(v) == int(16 + v * 219 / 255 + 0.5) for v in range(256)), "not expanded: 16 + round(v*219/255) for all 256 values")
check(all(cp.expanded_twice(v) == max(0, min(255, int((v - 16) * 255 / 219 + 0.5) if v >= 16 else 0)) for v in range(256)),
      "expanded twice: clamp(round((v-16)*255/219)) for all 256 values")
check(cp.not_expanded(0) == 16 and cp.not_expanded(255) == 235 and cp.expanded_twice(16) == 0 and cp.expanded_twice(235) == 255,
      "range end points: 0->16, 255->235, 16->0, 235->255")
check(max(abs(cp.expanded_twice(cp.not_expanded(v)) - v) for v in range(256)) <= 1, "limited round trip within 1 code (correct chain)")

with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    # --- deterministic, lossless, committed samples up to date -----------------------
    a, b = tmp / "a", tmp / "b"
    check(run("generate", "--out", a).returncode == 0 and run("generate", "--out", b).returncode == 0, "generate exits 0")
    names = sorted(p.name for p in a.iterdir())
    check(len(names) == 16 and names == sorted(p.name for p in b.iterdir()), "8 patterns, each a PNG and a manifest")
    check(all((a / n).read_bytes() == (b / n).read_bytes() for n in names), "generation is byte-identical between runs")
    committed = sorted(p.name for p in SAMPLES.glob("*-1920x1080.*"))
    check(committed == names and all((SAMPLES / n).read_bytes() == (a / n).read_bytes() for n in names),
          "docs/color-patterns matches the generator (regenerate with: color-patterns.py generate --out docs/color-patterns)")

    for png_path in sorted(a.glob("*.png")):
        data = png_path.read_bytes()
        m = json.loads((a / (png_path.stem + ".json")).read_text())
        w, h, rows = cp.read_png(data)
        ok = (w, h) == (m["width"], m["height"]) == (1920, 1080) and m["sha256"] == hashlib.sha256(data).hexdigest()
        ok = ok and data[12:16] == b"IHDR" and data[24:26] == b"\x08\x02" and b"sRGB" in data[:64]
        exact = True
        for r in m["regions"]:
            x0, y0 = r["x"] + r["w"] // 4, r["y"] + r["h"] // 4
            for y in range(y0, r["y"] + r["h"] - r["h"] // 4):
                row = rows[y]
                for x in range(x0, r["x"] + r["w"] - r["w"] // 4):
                    if list(row[x * 3:x * 3 + 3]) != r["file_rgb"]:
                        exact = False
                        break
                if not exact:
                    break
            if not exact:
                print("     region %s differs" % r["id"])
                break
        check(ok and exact, "%s: 8-bit RGB with sRGB chunk, hash in manifest, every region's centre holds file_rgb exactly" % png_path.stem)

    layout = json.loads((a / "c2-layout-1920x1080.json").read_text())
    regions = layout["regions"]
    check(sum(r["kind"] == "corner" for r in regions) == 4 and sum(r["kind"] == "edge" for r in regions) == 50,
          "layout: 4 labelled corners and 50 numbered edge segments (16 + 9 + 16 + 9)")
    vals = {tuple(r["file_rgb"]) for r in regions}
    check({(0, 0, 0), (255, 255, 255), (16, 16, 16), (235, 235, 235), (128, 128, 128), (255, 0, 0), (0, 255, 0), (0, 0, 255),
           (0, 255, 255), (255, 0, 255), (255, 255, 0), (235, 16, 16)} <= vals, "layout: black, white, gray, primaries, secondaries, 0/255 and 16/235")
    check(all(r["expect"]["correct"] == r["file_rgb"] for r in regions), "manifest: correct decode equals the file value")
    p16 = next(r for r in regions if r["label"] == "lim 16")
    check(p16["expect"]["expanded_twice"] == [0, 0, 0] and next(r for r in regions if r["label"] == "full 0")["expect"]["not_expanded"] == [16, 16, 16],
          "manifest: 16 collapses to 0 when expanded twice; 0 rises to 16 when not expanded")
    check(len({tuple(r["file_rgb"]) for r in regions if r["kind"] == "corner"}) == 4, "corners have four different colours")

    # --- check: simulated decodes at 1280x720 ------------------------------------------
    small = tmp / "small"
    run("generate", "--size", "1280x720", "--out", small)
    man = small / "c2-layout-1280x720.json"
    w, h, rows = cp.read_png((small / "c2-layout-1280x720.png").read_bytes())

    def variant(name, rows2, w2=w, h2=h, **kw):
        p = tmp / (name + ".png")
        p.write_bytes(encode(w2, h2, rows2, **kw))
        return run("check", "--manifest", man, p)

    r = variant("identity", rows, filters=(0, 1, 2, 3, 4))
    check(r.returncode == 0 and "range interpretation: correct" in r.stdout and "RESULT: PASS" in r.stdout,
          "identity (all five PNG filter types): correct, PASS")
    r = variant("rgba", rows, rgba=True)
    check(r.returncode == 0, "RGBA screenshot: alpha ignored, PASS")
    table = bytes(cp.not_expanded(v) for v in range(256))
    r = variant("not-expanded", [row.translate(table) for row in rows])
    check(r.returncode == 1 and "range interpretation: not_expanded" in r.stdout, "limited range not expanded: identified, FAIL")
    table = bytes(cp.expanded_twice(v) for v in range(256))
    r = variant("twice", [row.translate(table) for row in rows])
    check(r.returncode == 1 and "range interpretation: expanded_twice" in r.stdout, "full range expanded twice: identified, FAIL")
    swap = [b"".join(row[i + 2:i + 3] + row[i + 1:i + 2] + row[i:i + 1] for i in range(0, len(row), 3)) for row in rows]
    r = variant("swap", swap)
    check(r.returncode == 1 and "red/blue swapped regions: 0" not in r.stdout, "red and blue swapped: detected, FAIL")
    mirror = [b"".join(row[i:i + 3] for i in range(len(row) - 3, -1, -3)) for row in rows]
    r = variant("mirror", mirror)
    check(r.returncode == 1 and "orientation: mirrored left-right" in r.stdout, "mirrored picture: detected")
    r = variant("flip", rows[::-1])
    check(r.returncode == 1 and "orientation: flipped top-bottom" in r.stdout, "upside-down picture: detected")
    r = variant("rot", [row for row in mirror[::-1]])
    check(r.returncode == 1 and "orientation: rotated 180 degrees" in r.stdout, "rotated picture: detected")
    half = [bytes(row[x * 6:x * 6 + 3][i] for x in range(w // 2) for i in range(3)) for row in rows[::2]]
    r = variant("half", half, w // 2, h // 2)
    check(r.returncode == 0, "preview at half size (640x360): coordinates scale, PASS")
    noisy = [bytes(min(255, max(0, v + ((x * 7 + y * 13) % 5) - 2)) for x, v in enumerate(row)) for y, row in enumerate(rows)]
    r = variant("noise2", noisy)
    check(r.returncode == 0, "noise up to +/-2 codes: PASS at tolerance 3")
    worse = [bytes(min(255, max(0, v + 6)) for v in row) for row in rows]
    r = variant("off6", worse)
    check(r.returncode == 1 and "regions matching no interpretation: 0" not in r.stdout, "values 6 codes off: FAIL with the regions listed")

    # --- inputs it must refuse --------------------------------------------------------
    p16 = tmp / "deep.png"
    d = bytearray(encode(2, 2, [b"\0" * 6, b"\0" * 6]))
    d[24] = 16
    d[29:33] = struct.pack(">I", zlib.crc32(bytes(d[12:29])) & 0xffffffff)
    p16.write_bytes(bytes(d))
    r = run("check", "--manifest", man, p16)
    check(r.returncode == 2 and "only 8-bit RGB/RGBA" in r.stderr, "16-bit PNG refused (exit 2)")
    bad = tmp / "bad.png"
    raw = bytearray(encode(2, 2, [b"\0" * 6, b"\0" * 6]))
    raw[-5] ^= 1
    bad.write_bytes(bytes(raw))
    r = run("check", "--manifest", man, bad)
    check(r.returncode == 2 and "bad CRC" in r.stderr, "corrupt PNG refused (exit 2)")
    check(run("generate", "--size", "100x100", "--out", tmp / "x").returncode == 2, "too small a size refused")

print("%d failure(s)" % failures if failures else "all colour-pattern checks passed")
sys.exit(1 if failures else 0)
