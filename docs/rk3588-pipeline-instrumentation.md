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
| `decoded` / `decodeFailed` | worker returned an image / an error (may land in the next interval; over a stream `decoded + decodeFailed <= accepted`) |
| missed by driver sequence | gaps in `v4l2_buffer.sequence`, counted as missing frames |
| `restarts`, sequence resets | capture restarts; driver sequence going backwards or repeating |

"Dropped" in the log decision means every discard except decimation.

Stage timing is **accept to decoded-callback**, in userspace steady-clock
microseconds: p50/p95/p99 from a fixed 240-bucket log-linear histogram (8 buckets
per octave; the reported value is the bucket's upper bound, never below the true
value and at most 12.5% above it; exact below 8 us). It includes worker-thread
queuing and the hop back to the grabber thread, and excludes dequeue time. The
V4L2 kernel timestamp is never mixed in: it is a different clock.

## Unavailable is not zero

- Percentiles are `n/a` when there were no decoded frames in the interval.
- The missed-frames figure is `n/a` until the driver's sequence number has been
  seen to change (some drivers always report 0), so a driver that doesn't count
  can't look like a perfect stream.
- **LED output rate and DDP timing are not instrumented** and are printed as
  `n/a (not instrumented)`.

## Why LED and DDP metrics are not here

They need a frame identifier carried from the capture buffer through the
decoded image, the LED color calculation and the LED driver's send, and DDP
fragments one LED update into several packets that must be counted once. That
crosses `GrabberWorker`, the image signal path, `HyperHdrInstance` and
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

Once per interval, at Info level when something was lost (a discard other than
decimation, a decode error, a missed sequence, or an accounting mismatch) and at
Debug level otherwise:

```
capture stats: 30.0 fps dequeued, 1740 accepted, 1740 decoded, 0 decode errors; discarded 60 (decimation 60, worker busy 0, LUT loading 0, workers inactive 0, driver error 0, short payload 0, undersized 0); missed by driver sequence 0; accept->decoded p50/p95/p99 4.10/5.20/6.30 ms (1740 samples); restarts 1, sequence resets 0; LED output rate n/a, DDP timing n/a (not instrumented)
```

(Illustrative format, not a measurement.)

## Tests (hardware-free, ASan/UBSan in hosted CI; also run under TSan)

`tests/v4l2/stats_test.cpp`: normal capture; missing sequence numbers; worker
busy; software decimation; short frame; driver error and the other discard causes;
restart and sequence reset; counter reset; sequence wraparound; decode failure and
recovery; unavailable versus zero; histogram error bounds and percentile rank; a
200000-frame randomized stream checking the accounting identity at every
snapshot; and concurrent writers with a snapshotting reader.

## Not verified

- The edits in `V4L2Grabber.cpp` / `.h` were reviewed but not compiled in the
  development environment (no Qt). Hosted CI is the compile check.
- No measurement on the Orange Pi. Whether `workerBusy` or a long decode tail is
  the real bottleneck at 4K30 is exactly what a board run with this build will show.
