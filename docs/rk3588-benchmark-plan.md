# Native HDMI benchmark procedure

No RK3588 performance measurements were made for this branch. The user's earlier
600-frame V4L2 run demonstrates capture throughput, not HyperHDR or LED latency.

## Comparable configurations

| Case | Capture | Processing | Interpretation |
| --- | --- | --- | --- |
| Official stable HyperHDR | native HDMI if supported; otherwise a fixed USB capture source | stock CPU path | Record native discovery/start failure as unsupported, never 0 ms latency |
| Modified branch | native receiver (`snps_hdmirx` on mainline) BGR3, one-plane MPLANE | CPU, full then quarter output | Primary baseline for a future accelerator |
| Future RGA branch | identical native capture | RGA, matched output dimensions first | Separate result; not implemented here |

If official stable cannot capture native HDMI, a USB path is useful operationally
but includes different capture hardware/latency. Do not attribute that difference
solely to software. Also build the fork's original commit b1451f2 for a precise
source regression comparison on a working single-planar device. Preserve package,
commit, CMake cache, kernel, driver, EDID, Vertex2 configuration and WLED version.

Use the Orange Pi 5 Plus at 1080p60 from the same HDFury Vertex2 secondary output.
Keep source content, HDR/SDR mode, quantization, crop, LED layout/count, WLED/DDP,
network, brightness, signal detection, smoothing and tone mapping identical.
Disable software frame decimation for throughput runs. Start with tone mapping
and smoothing off; then measure the normal deployment configuration separately.
Do not run two consumers of the receiver node at once.

## Run schedule

Warm up for two minutes, measure five minutes, repeat five times per configuration
in alternating/random order. Record temperatures, clocks/governor and throttling
before/after each run; maintain cooling and background load. Measure full
1920x1080 output and the existing quarter option (960x540). For RGA first match
those sizes, then compare 320x180 and 160x90 where supported. Report output filter
and scaling stages: CPU quarter mode samples pixels; bilinear RGA output is not
mathematically identical and needs color/edge quality comparison.

Capture these alongside the HyperHDR performance dashboard and debug logs:

```sh
pid=$(pgrep -n -x hyperhdr)
pidstat -u -r -w -p "$pid" 1 300 > pidstat.txt
mpstat -P ALL 1 300 > mpstat.txt
# If the kernel grants perf permissions:
perf stat -p "$pid" -e task-clock,cycles,instructions,context-switches,cache-misses -- sleep 300
journalctl -u "hyperhdr@$(id -un)" --since '-7 minutes' > hyperhdr-journal.txt
```

Install `sysstat` and optionally `linux-perf`/the distribution's matching perf
package. CPU figures must state whether 100% means one core or the whole SoC.
Report RSS (median/peak), process CPU-seconds per wall-second, per-core load and
context switches. A perf permission error is missing data, not zero overhead.

## Throughput and loss

With HyperHDR stopped, independently repeat:

```sh
# The node number changes across boots; look it up by driver name.
RX=$(v4l2-ctl --list-devices | awk '/snps_hdmirx|rk_hdmirx/ {getline; print $1; exit}')
v4l2-ctl -d "$RX" --get-fmt-video --get-dv-timings
v4l2-ctl -d "$RX" --stream-mmap=4 --stream-count=18000 --stream-to=/dev/null
```

Record the reported rate and elapsed time. This establishes driver-only delivery.
Then stop v4l2-ctl and run HyperHDR. Its performance report measures processing
and good/bad worker frames; it does not by itself expose every captured frame or
kernel drop. For precise application capture statistics use an instrumented
benchmark build counting DQBUF calls, buffer.sequence gaps, ERROR flags, EAGAIN,
timeouts, short frames, worker-busy/software-decimation drops, worker completions
and QBUF failures. Emit aggregates once per second, not a full log per frame.
This instrumentation is future benchmark work, not present telemetry in this
branch. Treat stream restarts as new sequence epochs and account for sequence
wrap. Record actual V4L2 timestamp flags/domain before comparing clocks.

Report capture FPS (dequeues/time), processed FPS (completed frames/time), output
FPS (DDP frame groups/time), loss counts by cause, and p50/p95/p99 processing time.
For RGA separately time import/setup amortization, submission, fence wait, cache
sync, copies and CPU LED analysis. Include those costs in totals.

## Capture-to-LED and end-to-end latency

For software capture-to-DDP latency, instrument monotonic timestamps at successful
DQBUF, processing completion and final DDP send, associated by source sequence.
Avoid mixing driver SOE/EOF timestamps with host monotonic arrival times without
checking the driver contract. DDP send completion is not physical LED emission.
A packet capture on UDP 4048 helps distinguish network output rate and delays;
one frame may span multiple packets, so count completed groups, not packets.

For optical response, drive reproducible black/white and primary-color patches
on the HDMI source, with randomized transition intervals. Film the display patch
and corresponding LEDs together at 240 fps or faster, or use synchronized
photodiodes/oscilloscope channels. Define onset as a documented intensity threshold
(e.g. 50% of each transition) and keep exposure/threshold fixed. Record at least
100 transitions per run, including rising/falling edges. Report latency p50,
p95, p99 and uncertainty (240 fps resolves about 4.17 ms/frame). A shared camera
avoids cross-host clock synchronization but display scanout/panel response remains
part of the end-to-end result. A source-trigger/photodiode setup can isolate it.

## Recovery, image correctness and reporting

Run a 30-minute stream and ten cycles each of disconnect/reconnect, source off/on,
and timing change (1080p60 to 720p60 and back). Include a mode rejected by the
receiver. Record time to first valid preview and first correct LED output, retry
interval, CPU while signal is absent, RSS growth and fd/map counts after recovery.
Exercise start/stop and unsupported formats too. Use a single-planar USB device
for a regression run. No permanent readiness loop or stale frame buildup is
acceptable.

Check labeled corner/row patterns, red/green/blue fields, limited-range black 16
and white 235, gray ramps, padded rows and LED border sampling. Photograph settings
and keep preview frames. Report color correctness independently of speed.

Archive raw logs/CSV with columns: commit, kernel, run, capture mode, output size,
scale filter, range, HDR, smoothing, capture/processed/output FPS, loss categories,
CPU, RSS, temperature, p50/p95/p99 software and optical latency, recovery time.
Give medians and run-to-run ranges. Mark unavailable measurements explicitly.
Promote RGA only on measured CPU/latency benefit and equivalent acceptable image
quality and recovery behavior.
