#!/usr/bin/env python3
"""Colour reference patterns for checklist rows C1, C2 and C5 (issues #8, #10).

  color-patterns.py generate [--size 1920x1080] [--out DIR]
  color-patterns.py check --manifest M.json [--tolerance N] IMAGE.png

generate writes lossless 8-bit RGB PNG patterns and, for each, a manifest
(JSON) listing every region: position, the pixel values in the file, and what a
decoded capture of it should show under three range interpretations.

check samples a screenshot of the decoded image (for example HyperHDR's live
preview, cropped to the picture) at every region of a manifest and reports which
interpretation it matches: correct, limited range not expanded, or full range
expanded twice. It also detects swapped red/blue and mirrored or flipped
pictures. Exit status: 0 correct, 1 a problem was found or the result is
inconclusive, 2 usage or unreadable input.

Deterministic, standard library only (zlib for PNG): the same arguments always
give the same pixels and manifest regions. The PNG bytes, and so the manifest's
sha256, are byte-identical with the same zlib build; zlib and zlib-ng compress
differently.

What the pixel values mean, and what they don't
  The files hold sRGB code values, full range 0..255 per channel (PNG sRGB
  chunk). That is what a correct, colour-managed-off display path should *send*
  for each pixel when the source outputs full-range RGB. Showing a file does
  not choose the HDMI quantization: the source's driver or settings decide
  whether 0..255 is sent as is (full) or mapped to 16..235 (limited), and the
  display or receiver must be told which (AVI InfoFrame). A correct chain shows
  the file's values in either case. The patches at 16 and 235, and near black
  and near white, are there to tell the two wrong cases apart:
    not expanded    the receiver gets limited range, HyperHDR treats it as full:
                    v -> 16 + round(v * 219 / 255)   (black shows 16, white 235)
    expanded twice  the source sends full range, HyperHDR expands it as limited:
                    v -> clamp(round((v - 16) * 255 / 219))   (0..16 all show 0)
  Scaling, sharpening, HDR or "vivid" modes, colour management and lossy preview
  compression all change values; the instructions in
  docs/rk3588-color-patterns.md say how to avoid them.
"""
import argparse
import json
import pathlib
import struct
import sys
import zlib
import hashlib

SCHEMA = "hyperhdr.color-pattern/1"

# ------------------------------------------------------------------ range models


def not_expanded(v):
    return 16 + (v * 219 + 127) // 255


def expanded_twice(v):
    return max(0, min(255, ((v - 16) * 255 * 2 + 219) // (219 * 2)))


INTERPRETATIONS = {
    "correct": lambda rgb: list(rgb),
    "not_expanded": lambda rgb: [not_expanded(c) for c in rgb],
    "expanded_twice": lambda rgb: [expanded_twice(c) for c in rgb],
}

# ------------------------------------------------------------------ 5x7 font

FONT = {
    "0": "01110 10001 10011 10101 11001 10001 01110", "1": "00100 01100 00100 00100 00100 00100 01110",
    "2": "01110 10001 00001 00010 00100 01000 11111", "3": "11110 00001 00001 01110 00001 00001 11110",
    "4": "00010 00110 01010 10010 11111 00010 00010", "5": "11111 10000 11110 00001 00001 10001 01110",
    "6": "00110 01000 10000 11110 10001 10001 01110", "7": "11111 00001 00010 00100 01000 01000 01000",
    "8": "01110 10001 10001 01110 10001 10001 01110", "9": "01110 10001 10001 01111 00001 00010 01100",
    "A": "01110 10001 10001 11111 10001 10001 10001", "B": "11110 10001 10001 11110 10001 10001 11110",
    "C": "01110 10001 10000 10000 10000 10001 01110", "D": "11110 10001 10001 10001 10001 10001 11110",
    "E": "11111 10000 10000 11110 10000 10000 11111", "F": "11111 10000 10000 11110 10000 10000 10000",
    "G": "01110 10001 10000 10111 10001 10001 01111", "H": "10001 10001 10001 11111 10001 10001 10001",
    "I": "01110 00100 00100 00100 00100 00100 01110", "K": "10001 10010 10100 11000 10100 10010 10001",
    "L": "10000 10000 10000 10000 10000 10000 11111", "M": "10001 11011 10101 10101 10001 10001 10001",
    "N": "10001 11001 10101 10011 10001 10001 10001", "O": "01110 10001 10001 10001 10001 10001 01110",
    "P": "11110 10001 10001 11110 10000 10000 10000", "R": "11110 10001 10001 11110 10100 10010 10001",
    "S": "01111 10000 10000 01110 00001 00001 11110", "T": "11111 00100 00100 00100 00100 00100 00100",
    "U": "10001 10001 10001 10001 10001 10001 01110", "W": "10001 10001 10001 10101 10101 10101 01010",
    "Y": "10001 10001 01010 00100 00100 00100 00100", " ": "00000 00000 00000 00000 00000 00000 00000",
    "-": "00000 00000 00000 11111 00000 00000 00000",
}


class Canvas:
    def __init__(self, w, h, rgb=(0, 0, 0)):
        self.w, self.h = w, h
        self.px = bytearray(bytes(rgb) * (w * h))

    def rect(self, x, y, w, h, rgb):
        x0, y0, x1, y1 = max(0, x), max(0, y), min(self.w, x + w), min(self.h, y + h)
        row = bytes(rgb) * (x1 - x0)
        for yy in range(y0, y1):
            o = (yy * self.w + x0) * 3
            self.px[o:o + len(row)] = row

    def text(self, x, y, s, scale, rgb):
        for i, ch in enumerate(s):
            rows = FONT[ch].split()
            for ry, bits in enumerate(rows):
                for rx, b in enumerate(bits):
                    if b == "1":
                        self.rect(x + (i * 6 + rx) * scale, y + ry * scale, scale, scale, rgb)

    def get(self, x, y):
        o = (y * self.w + x) * 3
        return list(self.px[o:o + 3])


def ink(rgb):
    """Label colour that contrasts with rgb, avoiding the sampled centre anyway."""
    return (0, 0, 0) if sum(rgb) > 382 else (255, 255, 255)


# ------------------------------------------------------------------ PNG

def png(canvas):
    raw = b"".join(b"\x00" + bytes(canvas.px[y * canvas.w * 3:(y + 1) * canvas.w * 3]) for y in range(canvas.h))

    def chunk(t, data):
        return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", canvas.w, canvas.h, 8, 2, 0, 0, 0)) +
            chunk(b"sRGB", b"\x00") + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def read_png(data):
    """8-bit RGB or RGBA, non-interlaced (what screenshots and generate produce).
    Returns (width, height, rows of RGB bytes)."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG file")
    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        n, t = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if zlib.crc32(t + body) & 0xffffffff != struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])[0]:
            raise ValueError("PNG chunk %s has a bad CRC" % t.decode("latin-1"))
        if t == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif t == b"IDAT":
            idat += body
        elif t == b"IEND":
            break
        pos += 12 + n
    if not ihdr:
        raise ValueError("PNG without IHDR")
    w, h, depth, ctype, _, _, interlace = ihdr
    if depth != 8 or ctype not in (2, 6) or interlace:
        raise ValueError("only 8-bit RGB/RGBA non-interlaced PNG is supported (got depth %d, colour type %d, interlace %d)" % (depth, ctype, interlace))
    bpp = 3 if ctype == 2 else 4
    raw, stride, rows, prev = zlib.decompress(idat), w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            elif f != 0:
                raise ValueError("bad PNG filter %d" % f)
        rows.append(bytes(line) if bpp == 3 else bytes(v for i, v in enumerate(line) if i % 4 != 3))
        prev = line
    return w, h, rows


# ------------------------------------------------------------------ patterns

PALETTE = [("red", (255, 0, 0)), ("green", (0, 255, 0)), ("blue", (0, 0, 255)), ("cyan", (0, 255, 255)),
           ("magenta", (255, 0, 255)), ("yellow", (255, 255, 0)), ("white", (255, 255, 255)), ("black", (0, 0, 0))]
CORNERS = {"TL": (255, 255, 255), "TR": (255, 0, 0), "BL": (0, 255, 0), "BR": (0, 0, 255)}
EDGE_COLOURS = [(255, 0, 0), (255, 128, 0), (255, 255, 0), (0, 255, 0), (0, 255, 255), (0, 0, 255), (128, 0, 255), (255, 0, 255)]


def region(regions, kind, rid, label, x, y, w, h, rgb, discriminating=False):
    regions.append({"id": rid, "kind": kind, "label": label, "x": x, "y": y, "w": w, "h": h, "file_rgb": list(rgb),
                    "expect": {k: f(rgb) for k, f in INTERPRETATIONS.items()}, "discriminating": discriminating})


def layout(W, H):
    """C1/C2: labelled corners, edge segments, primaries/secondaries, black,
    white, gray, full-range 0/255 next to limited nominal 16/235, near-black and
    near-white steps, ramps."""
    c = Canvas(W, H, (64, 64, 64))   # background: no patch uses this gray
    regions = []
    s = max(1, W // 640)               # label scale: 3 at 1920
    band = 24 * s                      # edge band
    # Edge segments for LED zone mapping: numbered clockwise from the top-left.
    n_h, n_v = 16, 9
    k = 0
    for side, count in (("top", n_h), ("right", n_v), ("bottom", n_h), ("left", n_v)):
        for i in range(count):
            if side in ("top", "bottom"):
                seg = (W - 2 * band) // count
                x = band + (i if side == "top" else count - 1 - i) * seg
                y = 0 if side == "top" else H - band
                w, h = seg, band
            else:
                seg = (H - 2 * band) // count
                x = W - band if side == "right" else 0
                y = band + (i if side == "right" else count - 1 - i) * seg
                w, h = band, seg
            rgb = EDGE_COLOURS[k % len(EDGE_COLOURS)]
            c.rect(x, y, w, h, rgb)
            # Segment number near the corner, outside the sampled centre half.
            c.text(x + 2, y + 2, str(k + 1), max(1, s * 2 // 3), ink(rgb))
            region(regions, "edge", "E%02d" % (k + 1), "%s %d" % (side, i + 1), x, y, w, h, rgb)
            k += 1
    for name, rgb in CORNERS.items():
        x = 0 if name[1] == "L" else W - band
        y = 0 if name[0] == "T" else H - band
        c.rect(x, y, band, band, rgb)
        region(regions, "corner", name, {"TL": "top left", "TR": "top right", "BL": "bottom left", "BR": "bottom right"}[name], x, y, band, band, rgb)
        # Large label boxes just inside the band, in the corner's colour.
        lw, lh = 15 * 6 * s, 11 * s * 2
        lx = band + 4 * s if name[1] == "L" else W - band - 4 * s - lw
        ly = band + 4 * s if name[0] == "T" else H - band - 4 * s - lh
        c.rect(lx, ly, lw, lh, rgb)
        c.text(lx + 4 * s, ly + 4 * s, name + "-" + {"TL": "TOP LEFT", "TR": "TOP RIGHT", "BL": "BOT LEFT", "BR": "BOT RIGHT"}[name][:9], s, ink(rgb))
    # Patch rows inside the frame.
    x0, x1 = band + 40 * s, W - band - 40 * s
    rows = [
        [(n, rgb, False) for n, rgb in PALETTE],
        [("full 0", (0, 0, 0), True), ("lim 16", (16, 16, 16), True), ("gray 128", (128, 128, 128), False),
         ("lim 235", (235, 235, 235), True), ("full 255", (255, 255, 255), True),
         ("lim red", (235, 16, 16), True), ("lim green", (16, 235, 16), True), ("lim blue", (16, 16, 235), True)],
        [("black 4", (4, 4, 4), True), ("black 8", (8, 8, 8), True), ("black 24", (24, 24, 24), True), ("black 32", (32, 32, 32), True),
         ("white 224", (224, 224, 224), True), ("white 240", (240, 240, 240), True), ("white 248", (248, 248, 248), True), ("gray 192", (192, 192, 192), False)],
    ]
    top = band + 12 * s + 11 * s * 2
    ph = 36 * s
    gap = 4 * s
    for r, row in enumerate(rows):
        pw = (x1 - x0 - gap * (len(row) - 1)) // len(row)
        for i, (name, rgb, disc) in enumerate(row):
            x, y = x0 + i * (pw + gap), top + r * (ph + gap)
            c.rect(x, y, pw, ph, rgb)
            c.text(x + 2 * s, y + 2 * s, name.upper().replace("LIM", "L").replace("FULL", "F").replace("WHITE", "W").replace("BLACK", "K").replace("GRAY", "G")[:8], max(1, s - 1), ink(rgb))
            region(regions, "patch", "P%d%d" % (r + 1, i + 1), name, x, y, pw, ph, rgb, disc)
    # 17-step gray ramp (0, 16, ..., 255) and per-channel 9-step ramps.
    y = top + 3 * (ph + gap) + gap
    steps = [min(255, i * 16) for i in range(17)]
    pw = (x1 - x0) // 17
    for i, v in enumerate(steps):
        c.rect(x0 + i * pw, y, pw, ph, (v, v, v))
        region(regions, "ramp", "G%02d" % i, "gray step %d" % v, x0 + i * pw, y, pw, ph, (v, v, v), v in (0, 16, 240, 255))
    y += ph + gap
    for ch, cname in enumerate(("red", "green", "blue")):
        pw = (x1 - x0) // 9
        for i in range(9):
            v = min(255, i * 32)
            rgb = [0, 0, 0]
            rgb[ch] = v
            c.rect(x0 + i * pw, y, pw, ph // 2, rgb)
            region(regions, "ramp", "%s%d" % (cname[0].upper(), i), "%s step %d" % (cname, v), x0 + i * pw, y, pw, ph // 2, rgb)
        y += ph // 2 + gap
    return c, regions


def edges(W, H):
    """C5: only the edge segments and corners, centre black, so LED zones are
    easy to read on the strip."""
    full, regions = layout(W, H)
    c = Canvas(W, H, (0, 0, 0))
    keep = [r for r in regions if r["kind"] in ("edge", "corner")]
    for r in keep:
        c.rect(r["x"], r["y"], r["w"], r["h"], r["file_rgb"])
    return c, keep


def solid(name, rgb):
    def make(W, H):
        c = Canvas(W, H, rgb)
        regions = []
        region(regions, "patch", "S", name, W // 4, H // 4, W // 2, H // 2, rgb, rgb in ((0, 0, 0), (255, 255, 255)))
        return c, regions
    return make


PATTERNS = {"c2-layout": layout, "c5-edges": edges}
for _n, _rgb in (("red", (255, 0, 0)), ("green", (0, 255, 0)), ("blue", (0, 0, 255)), ("white", (255, 255, 255)),
                 ("black", (0, 0, 0)), ("gray128", (128, 128, 128))):
    PATTERNS["solid-" + _n] = solid(_n, _rgb)


def generate(out, W, H):
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    written = []
    for name, make in PATTERNS.items():
        canvas, regions = make(W, H)
        data = png(canvas)
        stem = "%s-%dx%d" % (name, W, H)
        (out / (stem + ".png")).write_bytes(data)
        manifest = {
            "schema": SCHEMA, "pattern": name, "file": stem + ".png", "width": W, "height": H,
            "sha256": hashlib.sha256(data).hexdigest(),
            "encoding": "8-bit RGB PNG, lossless, sRGB chunk; sRGB code values, full range 0-255 in the file",
            "orientation": "x right, y down; (0,0) is the top-left pixel",
            "interpretations": {
                "correct": "decoded = file value (full-range source, or limited-range source correctly expanded)",
                "not_expanded": "limited-range signal decoded as full: 16 + round(v*219/255)",
                "expanded_twice": "full-range signal expanded as limited: clamp(round((v-16)*255/219))",
            },
            "regions": regions,
        }
        (out / (stem + ".json")).write_text(json.dumps(manifest, indent=1) + "\n")
        written.append(stem)
    return written


# ------------------------------------------------------------------ check

def sample(rows, x, y, w, h):
    """Median of the centre half of a region."""
    height, width = len(rows), len(rows[0]) // 3 if rows else 0
    xs = range(max(0, x + w // 4), min(width, x + w - w // 4))
    ys = range(max(0, y + h // 4), min(height, y + h - h // 4))
    vals = [[], [], []]
    for yy in ys:
        row = rows[yy]
        for xx in xs:
            for ch in range(3):
                vals[ch].append(row[xx * 3 + ch])
    return [sorted(v)[len(v) // 2] for v in vals] if vals[0] else None


def close(a, b, tol):
    return all(abs(p - q) <= tol for p, q in zip(a, b))


def check(manifest, image, tol):
    W, H, rows = image
    sx, sy = W / manifest["width"], H / manifest["height"]
    results = []
    for r in manifest["regions"]:
        x, y = int(r["x"] * sx), int(r["y"] * sy)
        w, h = max(1, int(r["w"] * sx)), max(1, int(r["h"] * sy))
        got = sample(rows, x, y, w, h)
        matches = [k for k, v in r["expect"].items() if got and close(got, v, tol)]
        swapped = bool(got) and not matches and any(close(got[::-1], v, tol) for v in r["expect"].values())
        results.append({"id": r["id"], "label": r["label"], "kind": r["kind"], "got": got, "matches": matches,
                        "swapped_rb": swapped, "discriminating": r["discriminating"], "expect": r["expect"]["correct"]})
    # An interpretation explains the picture when every region matches it.
    verdict = {k: sum(1 for x in results if k in x["matches"]) for k in INTERPRETATIONS}
    candidates = [k for k, v in verdict.items() if v == len(results)]
    if candidates == ["correct"] or (len(candidates) == 1):
        range_result = candidates[0]
    elif "correct" in candidates:
        range_result = "not determined by this pattern"   # e.g. only 0/255 values: use c2-layout
    else:
        range_result = "inconclusive"
    best = candidates[0] if len(candidates) == 1 else "correct"
    # Orientation: each corner should show its own colour; a mirrored picture
    # shows the opposite corner's.
    corners = {x["id"]: x for x in results if x["kind"] == "corner"}
    orientation = "correct"
    if corners and not all("correct" in x["matches"] or best in x["matches"] for x in corners.values()):
        def shows(cid, other):
            exp = next(r for r in manifest["regions"] if r["id"] == other)["expect"]
            return corners[cid]["got"] and any(close(corners[cid]["got"], v, tol) for v in exp.values())
        if all(shows(a, b) for a, b in (("TL", "TR"), ("TR", "TL"), ("BL", "BR"), ("BR", "BL"))):
            orientation = "mirrored left-right"
        elif all(shows(a, b) for a, b in (("TL", "BL"), ("BL", "TL"), ("TR", "BR"), ("BR", "TR"))):
            orientation = "flipped top-bottom"
        elif all(shows(a, b) for a, b in (("TL", "BR"), ("BR", "TL"), ("TR", "BL"), ("BL", "TR"))):
            orientation = "rotated 180 degrees"
        else:
            orientation = "inconclusive"
    swapped = sum(1 for x in results if x["swapped_rb"])
    unmatched = [x for x in results if not x["matches"]]
    return {"range": range_result, "range_votes": verdict, "regions_checked": len(results), "orientation": orientation,
            "red_blue_swapped_regions": swapped, "unmatched": unmatched, "regions": results, "scale": [sx, sy]}


def report(res, tol):
    lines = ["range interpretation: %s (regions matching each: %s, of %d; tolerance %d)" % (
        res["range"], ", ".join("%s %d" % kv for kv in res["range_votes"].items()), res["regions_checked"], tol),
        "orientation: %s" % res["orientation"],
        "red/blue swapped regions: %d" % res["red_blue_swapped_regions"],
        "regions matching no interpretation: %d" % len(res["unmatched"])]
    for x in res["unmatched"][:20]:
        lines.append("  %s %-16s got %s, expected %s%s" % (x["id"], x["label"], x["got"], x["expect"], " (R/B swapped)" if x["swapped_rb"] else ""))
    ok = (res["range"] in ("correct", "not determined by this pattern") and res["orientation"] == "correct"
          and not res["red_blue_swapped_regions"] and not res["unmatched"])
    lines.append("RESULT: " + ("PASS" if ok else "FAIL or inconclusive: see above"))
    return "\n".join(lines) + "\n", ok


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate")
    g.add_argument("--size", default="1920x1080")
    g.add_argument("--out", default=".")
    c = sub.add_parser("check")
    c.add_argument("--manifest", required=True)
    c.add_argument("--tolerance", type=int, default=3)
    c.add_argument("--json", action="store_true")
    c.add_argument("image")
    a = ap.parse_args(argv)
    if a.cmd == "generate":
        try:
            W, H = (int(v) for v in a.size.lower().split("x"))
        except ValueError:
            print("--size must be WIDTHxHEIGHT", file=sys.stderr)
            return 2
        if not (640 <= W <= 7680 and 360 <= H <= 4320):
            print("--size must be between 640x360 and 7680x4320", file=sys.stderr)
            return 2
        for stem in generate(a.out, W, H):
            print(stem)
        return 0
    try:
        manifest = json.loads(pathlib.Path(a.manifest).read_text())
        if manifest.get("schema") != SCHEMA:
            raise ValueError("manifest schema %r, expected %r" % (manifest.get("schema"), SCHEMA))
        image = read_png(pathlib.Path(a.image).read_bytes())
    except (OSError, ValueError, zlib.error) as e:
        print("color-patterns: %s" % e, file=sys.stderr)
        return 2
    res = check(manifest, image, a.tolerance)
    text, ok = report(res, a.tolerance)
    sys.stdout.write(json.dumps(res, indent=1) + "\n" if a.json else text)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
