# RK3588 native HDMI: operator checklist

A run sheet for the Orange Pi 5 Plus checks that need a person and the physical
board. Each item says **what kind of action it is**, the exact commands, what to
look for, and where to attach the evidence. Background and troubleshooting:
[operator guide](rk3588-hardware-validation.md), [board runner](rk3588-board-runner.md),
[instrumentation](rk3588-pipeline-instrumentation.md), [benchmark reporting](rk3588-benchmark-reporting.md).

Nothing here is triggered automatically. No result counts as validated until its
evidence is attached to the issue. For the order of a session, and how to verify
a downloaded board artifact, see [next board session](rk3588-board-session.md).

| Class | Meaning |
|---|---|
| **A: safe while HyperHDR runs** | Read-only. Doesn't open the receiver for streaming, change settings or interrupt capture. May add CPU load. |
| **B: exclusive receiver** | Needs HyperHDR stopped (`sudo systemctl stop hyperhdr@ryan`) because it streams or allocates buffers on the receiver. Restart it afterwards. |
| **C: user-visible** | HyperHDR keeps running, but the TV picture, the source or the LEDs change (patterns, cable pulls, colour effects). |

Set the receiver node once per session (never assume `/dev/video0`):

```sh
RX=$(v4l2-ctl --list-devices | awk '/snps_hdmirx|rk_hdmirx/ {getline; print $1; exit}'); echo "$RX"
```

## A: safe while HyperHDR runs

| # | Check | How | Pass / record | Issue |
|---|---|---|---|---|
| A1 | Native ARM64 test suites + benchmark | Actions → **RK3588 board checks** → Run workflow on `master`, `stage: tests`, approve `rk3588-hil` | All sanitizer suites pass. Benchmark exits 0 on A76 and A55. Attach `bgr24-bench-*.txt` (and once #40 is merged `bgr24-bench-*.json` and `bgr24-bench-summary.md`) | #12, #9, #18 |
| A2 | Read-only receiver evidence | Same workflow, `stage: probe`, `expbuf: false`; or on the board `scripts/rk3588/collect-evidence.sh` | Archive written. Summary lists receiver, format, quantization, DV timings, holders of the node | #8, #10, #13 |
| A3 | Diagnostics | `scripts/diagnostics/hyperhdr-doctor.sh` | No failures, or each failure has its suggested fix noted | #27 |
| A4 | Receiver state, queried | `v4l2-ctl -d "$RX" -C power_present; v4l2-ctl -d "$RX" --query-dv-timings; v4l2-ctl -d "$RX" --get-fmt-video` | Record the source timing (`QUERY_DV_TIMINGS`) and the configured capture format separately; they are not interchangeable | #8 |
| A5 | Capture statistics | `journalctl -u hyperhdr@ryan --since "-15 min" \| grep "capture stats"` | One line per 60 s while capturing. `dequeued` ≈ source rate. `ACCOUNTING MISMATCH` never appears. Record `worker busy`, `decode errors`, `missed by driver sequence` and the accept→decoded p50/p95/p99 | #9, #11, #12 |
| A6 | Range evidence | `journalctl -u hyperhdr@ryan -b \| grep -E "RGB range (evidence\|mismatch)"` | One evidence line per start. Note any mismatch with the source model and its output range setting | #10 |
| A7 | Resource baseline | `P=$(pgrep -x hyperhdr); ps -o pid,rss,pcpu,etime -p "$P"; sudo ls /proc/"$P"/fd \| wc -l; sudo wc -l </proc/"$P"/maps` (`-x` matches the daemon only, not `hyperhdr-doctor.sh` or other command lines; `/proc/PID/fd` of another user needs root; read-only) | Record before C3/C4 and compare after: RSS, fd and mapping counts should return to the same range | #11 |
| A8 | LED network traffic (read-only) | `sudo tcpdump -i any -c 40 -n udp port 4048` for the `ddp` driver, `udp port 21324` for the `wled` driver (capture only); WLED `GET http://<wled>/json/info` | Packets at roughly the LED refresh rate, to the right address. Don't change WLED settings from here | #13 |
| A9 | LED output statistics | `journalctl -u hyperhdr@ryan --since "-15 min" \| grep "LED output stats"` (needs the LED device at debug log level for healthy intervals) | One line per minute: updates/s, new colours vs repeats, packets per update, `failed 0`, `rejected by socket 0`. Record next to A5's `capture stats` for the same minutes; the two rates are clocked independently. See [LED output statistics](led-output-stats.md) | #9, #13 |

A1 occupies one A76 and one A55 core for a few minutes. HyperHDR keeps running,
but its frame timing may degrade during that window; the summary marks load above 1.0.

## B: exclusive receiver (HyperHDR stopped)

Do these in one maintenance window. Note the stop and start times in the issue.

| # | Check | How | Pass / record | Issue |
|---|---|---|---|---|
| B1 | Kernel-level stream rate | `sudo systemctl stop hyperhdr@ryan`; `v4l2-ctl -d "$RX" --stream-mmap=4 --stream-count=600 --stream-to=/dev/null` | About 60 fps at 1080p60 with no buffer errors. Record the reported rate | #8 |
| B2 | Stream + DMA-BUF export probe | `scripts/rk3588/collect-evidence.sh --stream 600 --expbuf`; or the workflow with `stage: probe`, `expbuf: true`, and `host_idle_confirmed` set **by you** after stopping the service. The input is your attestation; nothing checks it independently | Stream result and whether `VIDIOC_EXPBUF` succeeds (RGA/zero-copy prerequisite) | #8, #13 |
| B3 | Restart | `sudo systemctl start hyperhdr@ryan`; then A5 and A6 again | Capture resumes; first stats line within ~60 s | — |

## C: user-visible (HyperHDR running)

| # | Check | How | Pass / record | Issue |
|---|---|---|---|---|
| C1 | HDMI preview: orientation and channel order | Show `docs/color-patterns/c2-layout-1920x1080.png` full-screen, 1:1 ([procedure](rk3588-color-patterns.md#board-session-procedure)); save the live preview as PNG, cropped to the picture; `python3 scripts/rk3588/color-patterns.py check --manifest docs/color-patterns/c2-layout-1920x1080.json preview.png` | `orientation: correct`, `red/blue swapped regions: 0`; corner labels readable. Attach screenshot and output | #10 |
| C2 | Full vs limited range references | Same pattern and `check` command as C1, once per source RGB range setting (**full**, **limited**, automatic if offered). Record the source setting and A4/A6 output each time ([how to find the setting](rk3588-color-patterns.md#2-find-and-record-the-sources-rgb-range-setting)) | `range interpretation: correct` and `RESULT: PASS` in **every** setting. `not_expanded` (black shows 16) or `expanded_twice` (0 and 16 both show 0) identifies the fault. Showing a file doesn't force the HDMI range; the source setting does | #10, #12 |
| C3 | No signal and reconnect | 10x each: unplug/replug HDMI; source power off/on; change timing (1080p ↔ 4K) and back. Plus one HyperHDR start with the source absent | `... capture paused, checking every ...` on loss; `Input signal on ... starting capture` on return; `Capture recovered after N failed attempt(s)` after failures; no stale preview; stats resume. Then A7 again | #11, #19 |
| C4 | Recovery soak | ≥ 30 min continuous video (overnight preferred) with C3 events every ~10 min | Stats every 60 s; no growth in RSS/fd/maps (A7 before and after); no `ACCOUNTING MISMATCH`; restarts only at the events. Once #39 is merged: no `decoding: N consecutive frames failed` unless frames really don't decode | #11, #19 |
| C5 | WLED/DDP colours | `docs/color-patterns/c5-edges-1920x1080.png`, then each `solid-*` image (or HyperHDR's solid-colour effect), then video | Each LED zone shows its edge segment's colour (manifest lists them); solids: every LED lit, right channel order, black off. Photograph the strip; pair with A9. A correct preview alone does not validate DDP | #13 |
| C6 | Single-planar path unchanged | Repeat C3 with a USB grabber instead of the receiver | Same log sequence; capture API logged as `single-planar` | #11 |

## After each session

- Attach the evidence archive (A2) from the start and end of the session.
- Put the exact commit (`hyperhdr --version` or the package SHA in A2) on every result.
- Mark only the rows with evidence as done in [the operator guide's status table](rk3588-hardware-validation.md#status).
