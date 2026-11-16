# V4L2 pipeline instrumentation (issue #9)

What the capture side of HyperHDR now counts, why it is built this way, and what
it deliberately does not claim. Related: #8, #11, #12; `docs/rk3588-benchmark-plan.md`.

## Decision: a narrow V4L2 collector, not an extension of `Grabber::frameStat`

| Option | Verdict |
| --- | --- |
| A. Extend `Grabber::frameStat` and `PerformanceReport` | Rejected. `frameStat` is shared by every capture backend (Windows MF, macOS AVF, X11, framebuffer...) and `PerformanceReport` has four fixed numeric fields that the web UI and API consume. Adding drop causes, sequence gaps and percentiles there changes all backends and the wire format for one driver's needs. |
| B. A narrowly scoped V4L2 collector | **Chosen.** `include/grabber/linux/v4l2/V4L2Stats.h`: header-only, Qt-free, used only by `V4L2Grabber`, tested without hardware (`tests/v4l2/stats_test.cpp`). No other backend, API field or UI changes. |

`frameStat` and the once-a-minute `PerformanceReport` are untouched. The new
collector reports on its own once per 60 s interval.

Known limitation of the existing statistics (not changed here): the average frame
time uses `InternalClock::nowPrecise()`, which has millisecond resolution and is
backed by `high_resolution_clock`, which isn't guaranteed steady. That is too
coarse for decodes that take 0.6-60 ms, so the new durations use the steady
microsecond clock (`InternalClock::nowMicro()`).

## What is counted

One interval is about 60 seconds. Every dequeued buffer resolves to exactly one
outcome, so `dequeued == discarded + accepted` (checked at each report; a
mismatch is printed as `ACCOUNTING MISMATCH`).

| Counter | Meaning |
| --- | --- |
| `dequeued` | buffers returned by `VIDIOC_DQBUF` |
| `driverError` | `V4L2_BUF_FLAG_ERROR` set; requeued |
| `shortPayload` | BGR24 payload smaller than the negotiated frame; requeued |
| `decimated` | skipped by the software frame decimation setting (a setting, not a loss) |
| `undersized` | `process_image`'s own size check failed (non-BGR24 formats) |
| `workersInactive` | workers not running (stop/restart in progress) |
| `lutNotReady` | a LUT is needed but still loading |
| `workerBusy` | no idle decode worker |
| `accepted` | handed to a worker |
| `decoded` / `decodeFailed` | worker returned an image / an error (may land in the next interval) |
| `abandoned` | the worker finished after capture restarted, so its result belongs to the old stream and is discarded. Over a stream, `decoded + decodeFailed + abandoned <= accepted`; the difference is frames still in a worker |
| missed by driver sequence | gaps in `v4l2_buffer.sequence`, counted as missing frames |
| `restarts`, sequence resets, duplicate sequences | capture restarts; the driver sequence going backwards; the same sequence twice (only counted once the driver's sequence has been seen to move) |

"Dropped" in the log decision means every discard except decimation.

Stage timing is **accept to decoded-callback**, in userspace steady-clock
microseconds. Each worker's timestamp slot records *which frame* it timed; a
completion is timed only if it is for that frame and only once, so a stale,
duplicate or out-of-order callback never borrows another frame's start time
(`AcceptTimes`). Results: p50/p95/p99 from a fixed 240-bucket log-linear histogram (8 buckets
per octave; the reported value is the bucket's upper bound, never below the true
value and at most 12.5% above it; exact below 8 us). It includes worker-thread
queuing and the hop back to the grabber thread, and excludes dequeue time. The
V4L2 kernel timestamp is never mixed in: it is a different clock.

## Unavailable is not zero

- Percentiles are `n/a` when there were no decoded frames in the interval.
- The missed-frames figure is `n/a` until the driver's sequence number has been
  seen to change (some drivers always report 0), so a driver that doesn't count
  can't look like a perfect stream. Such a driver also reports 0 duplicates.
- **Capture-to-LED latency is not instrumented** and is printed as
  `n/a (not instrumented)`. The LED output rate is counted separately, per LED
  device: see [LED output statistics](led-output-stats.md).

## Why capture-to-LED latency is not here

It needs a frame identifier carried from the capture buffer through the decoded
image, the LED color calculation and the LED driver's send. The per-device output
counters ([LED output statistics](led-output-stats.md)) count updates and packets
(one DDP update can be several packets, counted once), but carry no frame
identifier, and with smoothing an LED update blends several captures. Linking a
capture to its LED update crosses `GrabberWorker`, the image signal path, `HyperHdrInstance` and
`LedDevice`, which is a larger refactor than a counter and easy to get subtly
wrong. Doing it as a fragile shortcut would produce numbers that look precise but
aren't. Proposed follow-up: a monotonic `frameId` assigned at accept time,
propagated with the image, and a per-instance latch recording when the first
packet of the update carrying that id leaves the DDP driver.

## Sequence rules

`delta = int32(seq - last)`, so wraparound at 2^32 is a +1 step. A forward step
larger than 1 adds `delta - 1` missed frames; a repeat or a backwards step is not
a gap (it is recorded as a duplicate or a reset and tracking re-anchors); a
capture restart clears the anchor first so the driver's new sequence from 0 isn't
read as a gap. Counters reset every interval but sequence tracking continues, so
an interval boundary neither invents nor hides a gap.

## Overhead

All counters are relaxed atomics: nothing allocates, memory is fixed
(`sizeof(Stats)` = 2056 bytes), and the logger is only called once per interval.
Measured on an x86_64 development machine, single thread: about 27 ns per frame
for dequeue + sequence + accept + decode-with-duration. That is not an RK3588
number, and was not measured on the board.

## Reading the log

Once per interval, and once more when capture is torn down: on a stop, a
restart after a stall, signal loss, source change or error, and at shutdown. The
partial interval is reported then, so it isn't lost when frames stop arriving.
The level comes from `reportLevel()`:
- **nothing** when no frame moved. A failed start during backoff stays silent.
- **Info** when something was lost: a discard other than decimation, a decode
  error, a missed sequence, or an accounting mismatch.
- **Debug** otherwise. Frames abandoned by a restart and decimated frames are
  expected, so they don't raise the level.

Torn-down intervals are labelled `capture stats (capture stopped): ...`.

```
capture stats: 30.0 fps dequeued, 1740 accepted, 1740 decoded, 0 decode errors, 0 abandoned by restart; discarded 60 (decimation 60, worker busy 0, LUT loading 0, workers inactive 0, driver error 0, short payload 0, undersized 0); missed by driver sequence 0; accept->decoded p50/p95/p99 4.10/5.20/6.30 ms (1740 samples); restarts 1, sequence resets 0, duplicate sequences 0; LED output: see "LED output stats"; capture-to-LED latency n/a (not instrumented)
```

(Illustrative format, not a measurement.)

## Tests (hardware-free, ASan/UBSan in hosted CI; also run under TSan)

`tests/v4l2/stats_test.cpp`: normal capture; missing sequence numbers; worker
busy; software decimation; short frame; driver error and the other discard causes;
restart and sequence reset; counter reset; sequence wraparound; decode failure and
recovery; unavailable versus zero; histogram error bounds and percentile rank; a
200000-frame randomized stream checking the accounting identity at every
snapshot; and concurrent writers with a snapshotting reader. Hardening tests:
- frames abandoned by a restart
- drivers without sequence numbers
- every report level, including silent restarts-only intervals and an accounting mismatch
- the flush label
- frame-keyed acceptance times (stale, duplicate, out-of-order, unknown worker, clock step back, teardown, worker re-creation)
- a 50000-step lifecycle with restarts, checking that every accepted frame ends decoded, failed or abandoned, and that each decode is timed exactly once

## Not verified

- The edits in `V4L2Grabber.cpp` / `.h` were reviewed but not compiled in the
  development environment (no Qt). Hosted CI is the compile check.
- No measurement on the Orange Pi. Whether `workerBusy` or a long decode tail is
  the real bottleneck at 4K30 is exactly what a board run with this build will show.
