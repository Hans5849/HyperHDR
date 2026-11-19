#!/usr/bin/env python3
"""Pair HyperHDR `capture stats` and `LED output stats` lines from the same minutes.

  stats-align.py [--json] [--min-overlap 0.9] JOURNAL

JOURNAL is HyperHDR's journal with ISO timestamps, for example
  journalctl -u hyperhdr@USER -o short-iso-precise --since "-20 min" > journal.txt
or logs/hyperhdr-journal.txt from an evidence archive (collect-evidence.sh).
`-` reads standard input. Standard library only: reads a file, runs nothing.

For each LED device and each capture interval it reports the two side by side
when they cover the same time, and says why when they don't. It never combines
them into a pipeline figure: the capture rate counts frames the receiver
delivered, the LED rate counts LED updates written by one device. With
smoothing, LED updates follow the LED refresh clock, not the frames
(docs/led-output-stats.md, docs/rk3588-frame-stages.md). Capture-to-LED latency
is not instrumented, so it is always reported as unmeasured.

Interval reconstruction. Each line is written when its interval ends; the journal
time is that end (1 s resolution with -o short-iso, 1 us with short-iso-precise).
  LED output stats   prints its length ("60.0 s"): start = end - length.
  capture stats      doesn't print its length. It is recovered from the counts:
                     dequeued = accepted + discarded (the line's own identity),
                     length = dequeued / (fps dequeued). The rate is rounded to
                     0.1 fps, so the start is uncertain by dequeued/(fps +- 0.05);
                     the uncertainty is printed. With no dequeued frames, a rate
                     that rounds to 0.0, or an ACCOUNTING MISMATCH, the length is
                     unknown; at low rates it may be too uncertain to pair (its
                     half-range exceeds 1 - --min-overlap of the length). Either
                     way the interval is reported and not paired.
Two intervals are paired when their overlap covers at least --min-overlap of
each (default 0.9).

Healthy intervals of both lines are logged at debug level. If a kind of line is
missing, the report says so, and why it may be (log level, nothing captured, LED
device off): missing lines are never read as zero.

Exit status: 0 at least one pair, 1 none, 2 usage or unreadable input.
"""
import argparse
import datetime as dt
import json
import re
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")
TS = re.compile(r"^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:[.,]\d+)?)(Z|[+-]\d\d:?\d\d)?\s")
# HyperHDR's own prefix: "hh:mm:ss.zzz LOGGER_NAME (padded) : <LEVEL> [file:line:function() | ]message"
MSG = re.compile(r"\d\d:\d\d:\d\d\.\d{3} (\S+)\s*: <(\w+)> (?:[^|]*\(\) \| )?(capture stats|LED output stats)(?: \(([^)]*)\))?: (.*)$")
INT = r"(\d+)"

CAPTURE = re.compile(
    r"([\d.]+) fps dequeued, %s accepted, %s decoded, %s decode errors, %s abandoned by restart; discarded %s "
    r"\(decimation %s, worker busy %s, LUT loading %s, workers inactive %s, driver error %s, short payload %s, undersized %s\); "
    r"missed by driver sequence (\d+|n/a); accept->decoded p50/p95/p99 ([\d.]+|n/a)/([\d.]+|n/a)/([\d.]+|n/a) ms \(%s samples\); "
    r"restarts %s, sequence resets %s, duplicate sequences %s" % ((INT,) * 16))
CAPTURE_KEYS = ["fps", "accepted", "decoded", "decode_errors", "abandoned", "discarded", "decimation", "worker_busy", "lut_loading",
                "workers_inactive", "driver_error", "short_payload", "undersized", "missed", "p50_ms", "p95_ms", "p99_ms",
                "duration_samples", "restarts", "sequence_resets", "duplicate_sequences"]
LED_HEAD = re.compile(r"([\d.]+) s(?:, (.*))?$")
LED_FIELDS = {
    "presented": re.compile(r"presented (\d+)"),
    "updates": re.compile(r"; updates (\d+)"),
    "new_colours": re.compile(r"new colours (\d+)"),
    "repeats": re.compile(r"repeats (\d+)"),
    "failed": re.compile(r", failed (\d+)"),
    "while_inactive": re.compile(r", (\d+) while inactive"),
    "superseded": re.compile(r", (\d+) superseded"),
    "sent_no_packet": re.compile(r", (\d+) sent no packet"),
    "packets": re.compile(r"; packets (\d+)"),
    "max_packets_per_update": re.compile(r"max (\d+)\)"),
    "rejected_by_socket": re.compile(r"rejected by socket (\d+)"),
    "bytes_accepted": re.compile(r"(\d+) bytes accepted"),
}
# Driver type (from the LEDDEVICE_<TYPE> logger) -> what its packets are.
TRANSPORT = {
    "DDP": "ddp: DDP on UDP 4048",
    "WLED": "wled: WLED realtime UDP (DRGB/DNRGB), port from WLED's info, 21324 by default; not DDP",
}


class InputError(Exception):
    pass


def parse_time(text, zone):
    text = text.replace(",", ".")
    if zone and zone != "Z" and ":" not in zone:
        zone = zone[:3] + ":" + zone[3:]
    t = dt.datetime.fromisoformat(text + ("+00:00" if zone == "Z" else (zone or "")))
    return t


def num(v):
    if v is None or v == "n/a":
        return None
    return float(v) if "." in v else int(v)


def parse(lines):
    """Returns (capture intervals, LED intervals per device, notes)."""
    capture, led, notes = [], {}, []
    no_time = 0
    for raw in lines:
        line = ANSI.sub("", raw.rstrip("\n"))
        m = MSG.search(line)
        if not m:
            continue
        logger, level, kind, context, body = m.groups()
        tm = TS.match(line)
        if not tm:
            no_time += 1
            continue
        end = parse_time(*tm.groups())
        if kind == "capture stats":
            c = CAPTURE.match(body)
            if not c:
                notes.append("unrecognised capture stats line (format changed?): %s" % body[:80])
                continue
            rec = {k: num(v) for k, v in zip(CAPTURE_KEYS, c.groups())}
            rec.update(kind="capture", logger=logger, level=level, context=context, end=end,
                       mismatch="ACCOUNTING MISMATCH" in body)
            dequeued = rec["accepted"] + rec["discarded"]
            rec["dequeued"] = dequeued
            # Why the length can't be recovered, if it can't.
            rec["unknown_length"] = ("its counts don't add up (ACCOUNTING MISMATCH)" if rec["mismatch"]
                                     else "no frames were dequeued" if not dequeued
                                     else "%d frames at a rate that rounds to 0.0 fps" % dequeued if not rec["fps"]
                                     else None)
            if rec["unknown_length"]:
                rec["seconds"] = rec["seconds_lo"] = rec["seconds_hi"] = None
            else:
                rec["seconds"] = dequeued / rec["fps"]
                rec["seconds_lo"] = dequeued / (rec["fps"] + 0.05)
                rec["seconds_hi"] = dequeued / (rec["fps"] - 0.05) if rec["fps"] > 0.05 else None
            capture.append(rec)
        else:
            h = LED_HEAD.match(body)
            if not h:
                notes.append("unrecognised LED output stats line (format changed?): %s" % body[:80])
                continue
            rest = h.group(2) or ""
            rec = {k: (int(r.search(rest).group(1)) if r.search(rest) else None) for k, r in LED_FIELDS.items()}
            device = logger[len("LEDDEVICE_"):] if logger.startswith("LEDDEVICE_") else logger
            rec.update(kind="led", logger=logger, device=device, level=level, context=context, end=end,
                       seconds=float(h.group(1)), idle=rest.startswith("no LED output"),
                       packets_counted="packets n/a" not in rest)
            rec["seconds_lo"] = rec["seconds_hi"] = rec["seconds"]
            led.setdefault(device, []).append(rec)
    if no_time:
        notes.append("%d stats line(s) without an ISO journal timestamp ignored: use journalctl -o short-iso-precise" % no_time)
    return capture, led, notes


def span(r):
    if r["seconds"] is None:
        return None
    return r["end"] - dt.timedelta(seconds=r["seconds"]), r["end"]


def overlap(a, b):
    sa, sb = span(a), span(b)
    if not sa or not sb:
        return None
    o = (min(sa[1], sb[1]) - max(sa[0], sb[0])).total_seconds()
    if o <= 0:
        return 0.0
    return min(o / a["seconds"], o / b["seconds"]) if a["seconds"] and b["seconds"] else 0.0


def too_uncertain(c, min_overlap):
    """A capture length known only to within more than the overlap tolerance.

    The rate is printed to 0.1 fps, so at low rates the recovered length is
    uncertain (e.g. 6 frames at 0.1 fps: 40..120 s). Pairing such an interval
    would claim an overlap that the data can't support."""
    if c["seconds"] is None:
        return False
    if c["seconds_hi"] is None:
        return True
    return (c["seconds_hi"] - c["seconds_lo"]) / 2 > (1 - min_overlap) * c["seconds"]


def align(capture, led, min_overlap):
    pairs = []
    for device, recs in sorted(led.items()):
        for c in capture:
            if too_uncertain(c, min_overlap):
                continue
            best, best_o = None, 0.0
            for l in recs:
                o = overlap(c, l)
                if o is not None and o > best_o:
                    best, best_o = l, o
            if best is not None and best_o >= min_overlap:
                pairs.append({"device": device, "capture": c, "led": best, "overlap": best_o})
    return pairs


def fmt_time(t):
    return t.isoformat(timespec="seconds")


def rate(count, seconds):
    return "n/a" if count is None or not seconds else "%.2f/s" % (count / seconds)


def ms(v):
    return "n/a" if v is None else "%.2f" % v


def report(capture, led, notes, pairs, min_overlap):
    out = []
    out.append("capture stats lines: %d; LED output stats lines: %s" % (
        len(capture), ", ".join("%s %d" % (d, len(r)) for d, r in sorted(led.items())) or "0"))
    if not capture:
        out.append("NOTE no capture stats lines. Healthy intervals are logged at debug level: set the V4L2 grabber's log level to debug "
                   "(otherwise only intervals with losses appear), and check that capture was running. Not evidence of zero frames.")
    if not led:
        out.append("NOTE no LED output stats lines. Healthy intervals are logged at debug level: set the LED device's log level to debug, "
                   "and check that the device was enabled. Not evidence of zero updates.")
    for device in sorted(led):
        out.append("LED device %s: %s" % (device, TRANSPORT.get(device, "packets counted only for drivers using HyperHDR's UDP provider")))
    for c in capture:
        if c["mismatch"]:
            out.append("WARN capture interval ending %s has ACCOUNTING MISMATCH: its length can't be recovered; not paired" % fmt_time(c["end"]))
        elif c["unknown_length"]:
            out.append("NOTE capture interval ending %s: %s, so its length is unknown; not paired" % (fmt_time(c["end"]), c["unknown_length"]))
        elif too_uncertain(c, min_overlap):
            hi = "%.1f" % c["seconds_hi"] if c["seconds_hi"] else "unbounded"
            out.append("NOTE capture interval ending %s: %d frames at %.1f fps, so its length is %.1f..%s s (the rate is rounded to 0.1 fps); "
                       "too uncertain to pair at %d%% overlap" % (fmt_time(c["end"]), c["dequeued"], c["fps"], c["seconds_lo"], hi, round(min_overlap * 100)))
    for n in notes:
        out.append("NOTE " + n)
    out.append("")
    if not pairs:
        out.append("No capture and LED intervals cover the same time (overlap >= %d%% of each)." % round(min_overlap * 100))
    for p in pairs:
        c, l = p["capture"], p["led"]
        cs, ls = span(c), span(l)
        unc = (c["seconds_hi"] - c["seconds_lo"]) / 2 if c["seconds_hi"] else None
        out.append("== %s .. %s (overlap %d%%), LED device %s" % (fmt_time(max(cs[0], ls[0])), fmt_time(min(cs[1], ls[1])), round(p["overlap"] * 100), p["device"]))
        out.append("   capture %s%s: %.1f s (+-%s s), %.1f fps dequeued (%d frames), %d accepted, %s decoded, %d decode errors, %d abandoned; "
                   "discarded %d (worker busy %d, decimation %d, driver error %d, short payload %d); missed by driver sequence %s; "
                   "accept->decoded p50/p95/p99 %s/%s/%s ms; restarts %d" % (
                       c["logger"], " (%s)" % c["context"] if c["context"] else "", c["seconds"], "%.1f" % unc if unc is not None else "?",
                       c["fps"], c["dequeued"], c["accepted"], rate(c["decoded"], c["seconds"]), c["decode_errors"], c["abandoned"],
                       c["discarded"], c["worker_busy"], c["decimation"], c["driver_error"], c["short_payload"],
                       "n/a" if c["missed"] is None else c["missed"], ms(c["p50_ms"]), ms(c["p95_ms"]), ms(c["p99_ms"]), c["restarts"]))
        if l["idle"]:
            led_text = "no LED output in this interval"
        else:
            pk = ("packets %d (%.2f/update, max %s), rejected by socket %s" % (l["packets"], l["packets"] / l["updates"] if l["updates"] else 0,
                                                                              l["max_packets_per_update"], l["rejected_by_socket"])
                  if l["packets_counted"] and l["packets"] is not None else "packets n/a (driver doesn't use the UDP provider)")
            led_text = "updates %s (new colours %s, repeats %s), failed %s; %s" % (
                rate(l["updates"], l["seconds"]), l["new_colours"], l["repeats"], l["failed"], pk)
        out.append("   LED     %s%s: %.1f s, %s" % (l["logger"], " (%s)" % l["context"] if l["context"] else "", l["seconds"], led_text))
    out.append("")
    out.append("Independent rates: frames delivered by the receiver vs LED updates written by one device. Not a pipeline throughput.")
    out.append("capture-to-LED latency: not measured (no frame identifier links them; see docs/rk3588-frame-stages.md).")
    out.append("WLED receipt and display: not measured (UDP has no acknowledgement).")
    return "\n".join(out)


def as_json(capture, led, notes, pairs):
    def clean(r):
        d = dict(r)
        d["end"] = fmt_time(r["end"])
        s = span(r)
        d["start"] = fmt_time(s[0]) if s else None
        return d
    return {
        "capture": [clean(c) for c in capture],
        "led": {k: [clean(r) for r in v] for k, v in led.items()},
        "pairs": [{"device": p["device"], "overlap": round(p["overlap"], 3), "capture_end": fmt_time(p["capture"]["end"]),
                   "led_end": fmt_time(p["led"]["end"])} for p in pairs],
        "notes": notes,
        "not_measured": ["capture-to-LED latency", "WLED receipt and display", "optical latency"],
    }


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("journal")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--min-overlap", type=float, default=0.9)
    a = ap.parse_args(argv)
    if not 0 < a.min_overlap <= 1:
        print("stats-align: --min-overlap must be in (0, 1]", file=sys.stderr)
        return 2
    try:
        f = sys.stdin if a.journal == "-" else open(a.journal, encoding="utf-8", errors="replace")
        with f:
            capture, led, notes = parse(f)
    except OSError as e:
        print("stats-align: %s" % e, file=sys.stderr)
        return 2
    pairs = align(capture, led, a.min_overlap)
    if a.json:
        print(json.dumps(as_json(capture, led, notes, pairs), indent=1, default=str))
    else:
        print(report(capture, led, notes, pairs, a.min_overlap))
    return 0 if pairs else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
