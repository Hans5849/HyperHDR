# Latency and throughput: what can be measured, and how

Issues #8, #9 and #12. This plan separates six things that are easy to blur:
1. kernel capture timestamps
2. accept → decode time
3. application LED update timing
4. local UDP send acceptance
5. the WLED controller receiving and displaying an update
6. optical end-to-end latency

Each one is measured differently, if it can be measured at all. A result names
which of the six it is. Stage definitions: [frame stages](rk3588-frame-stages.md).

**Nothing on this page has been measured on the board yet.** It describes
methods and tools.

## 1. What exists today

| # | Quantity | Available today | Instrument, clock, resolution | What it can't tell you |
|---|---|---|---|---|
| 1 | **Kernel capture timestamp** of each buffer (`v4l2_buffer.timestamp`) | **Not recorded by HyperHDR.** It reads `sequence` (gaps = frames the driver missed) and ignores the timestamp. Outside HyperHDR, `v4l2-ctl --stream-mmap --verbose` prints it per buffer (checklist B1, class B: HyperHDR stopped) | Kernel; the clock and whether it marks start or end of frame are in the buffer flags (`V4L2_BUF_FLAG_TIMESTAMP_*`, `TSTAMP_SRC_*`). Record them; don't assume | Anything about HyperHDR. Comparing it with HyperHDR's times would need the same clock domain: `InternalClock::nowMicro()` is `steady_clock` (`CLOCK_MONOTONIC` with libstdc++) but **offset from process start**, so the two aren't comparable as logged |
| 2 | **Accept → decoded** per frame | **Yes**, #32: p50/p95/p99 per 60 s interval in `capture stats` | Userspace steady clock, µs; log-linear histogram, reported value ≤ 12.5% above the true one | Dequeue time, kernel time, and anything after the decoded callback (LED colour calculation, smoothing, output) |
| — | Frames dequeued, accepted, decoded, discarded by cause, missed by driver sequence | **Yes**, #32, per interval | Counters | The source frame rate: `dequeued` is what reached HyperHDR. Compare with the source timing (`QUERY_DV_TIMINGS`, A2/A4) |
| 3 | **LED updates** written per device: presented, new colours vs repeats, failed | **Yes**, #45: `LED output stats` per 60 s | Counters and rates; no per-update timestamps | Update-to-update timing (jitter), and which captured frame an update came from: with smoothing on, updates follow the LED refresh timer, not the frames |
| 4 | **Local UDP send acceptance**: datagrams and bytes the socket accepted or rejected | **Yes**, #45, same line | Counters | Whether a datagram left the host or arrived. `sudo tcpdump -tt -n -i <if> udp port 4048` (ddp) or `21324` (wled) gives kernel send times per datagram (checklist A8, read-only): inter-update timing and jitter on the wire. That is still not receipt |
| 5 | **WLED receipt and display** | **No.** UDP has no acknowledgement | WLED's `GET /json/info` (read-only) gives `live` (realtime data currently arriving) and `leds.fps` (its own current output frame rate) | Per-update receipt, drops on the controller, or display time. `live: true` only says data is arriving |
| 6 | **Optical end-to-end latency** | **No** | Section 3 | — |

Never substitute one row for another:
- The LED update rate isn't the HDMI frame rate. With smoothing it is the LED
  refresh rate, whatever the source does.
- Packets aren't updates: DDP splits one update into several datagrams.
- Accept → decoded isn't capture-to-LED.
- An accepted packet isn't light.

To read rows 2–4 for the same minutes, use `scripts/rk3588/stats-align.py`
(checklist A5/A9), once the evidence-readiness change is merged.

## 2. Why there is no frame identifier yet

Capture-to-LED latency in software (from DQBUF to the first datagram of the
update carrying that frame) needs an identifier carried from the capture buffer
through the decoded image, `HyperHdrInstance`, smoothing and `LedDevice`. It is
deliberately not built yet:

- **It is undefined with smoothing on.** The default configuration blends
  several frames into each update, so "the update carrying frame N" doesn't
  exist. It is only meaningful with smoothing off, which isn't the deployed
  configuration.
- **It crosses five components** and every capture backend, for one driver's
  benefit (the same reason #32 kept its collector narrow).
- **It still stops at the socket.** It can't see the network, WLED or the LEDs,
  which is where users perceive latency.

The optical method below measures what users see, in the deployed
configuration, without changing HyperHDR. If software stage timing is wanted
later, the minimal design is:
1. a 64-bit id assigned at accept and stored with the decoded image;
2. propagated unchanged by the processing path, with smoothing off only;
3. one latch per LED device recording `nowMicro()` when the first datagram of
   the update computed from that id is accepted;
4. reported as percentiles beside `accept->decoded`, never per frame.

That design should be reviewed on its own issue before any code.

## 3. Optical latency: procedure

### Two numbers, kept apart

- **M1, perceived offset:** LED light minus TV light for the same transition.
  It's what a viewer sees: positive means the LEDs lag the picture. It includes
  the TV's own processing as a negative term, so it depends on the TV's picture
  mode (game mode or not).
- **M2, HDMI-to-LED latency:** LED light minus the moment the frame leaves the
  source. Measure the TV's input lag `L_TV` separately, at the same patch
  position and picture mode, with a dedicated HDMI input-lag tester. Then
  M2 = M1 + `L_TV`, and their uncertainties add (in quadrature for independent
  errors). If `L_TV` isn't measured, report M1 only and say so.

### Equipment

- **Recorder (preferred):** two photodiodes or phototransistors with load
  resistors, into one two-channel recorder: a USB oscilloscope, a logic
  analyser with analogue inputs, or a stereo audio interface at ≥ 44.1 kHz.
  Both channels on one recorder share one clock, so no cross-device
  synchronisation is needed.
- **Alternative:** a camera at ≥ 240 fps filming the TV patch and the LEDs in
  one frame, with onsets read frame by frame. Each onset is then quantised to
  one frame (4.17 ms at 240 fps), and the difference of two onsets is uncertain
  by up to ±1 frame (SD = frame/√6 ≈ 1.7 ms at 240 fps).
- **Sensor placement:** one sensor on the TV patch, shielded from ambient light
  and from the LEDs; one on an LED inside the zone that samples that patch.
- **Same physical setup as the board session:** HDFury Vertex2 secondary output
  to the receiver, the same LED layout and controller.

### Source pattern

```sh
python3 scripts/rk3588/optical-latency.py schedule --seed 1 --count 200 out/
ffmpeg -f concat -safe 0 -i out/pattern.txt -vf "fps=60,format=yuv420p" -c:v libx264 -crf 0 -g 60 timing-pattern.mp4
```

- **Frames:** full-frame black/white, alternating, each segment a random whole
  number of frames between 300 and 1500 ms. Random lengths stop transitions
  locking to the LED refresh or smoothing period.
- **Each run gives:** 200 transitions (100 rising, 100 falling) and
  `schedule.csv`, to check that every transition was recorded.
- **Playback:** at the source's native rate, from a player that doesn't drop
  frames.
- **Colour delays:** for channel-specific delays, repeat with a primary colour
  instead of white.

### Sampling and analysis

- **Recorder:** sample both channels at ≥ 10 kHz (0.1 ms) and export
  `time_s,ref,led` CSV.
- **Analysis:** `python3 scripts/rk3588/optical-latency.py analyze recording.csv`
  (`--json` for archiving).
- **Envelope:** a moving maximum over 3 ms hides LED and backlight PWM. Both
  channels get the same filter, so its delay cancels in the difference; the
  remaining error is at most one PWM period.
- **Onset:** the 50% crossing between each channel's settled levels. Smoothing
  turns the LED edge into a ramp, so the 50% point is part of the result: state
  the smoothing settings with it.
- **Edges:** rising and falling are reported separately, then together.
- **Statistics:** n, p50/p95/p99, mean, SD, min/max, and a reproducible
  bootstrap 95% interval of the median. p99 is flagged when n < 500.
- **Unpaired transitions:** a TV transition with no LED response, or an LED
  transition without one, is listed, never dropped. Investigate any before
  quoting a number.
- **Camera onsets:** use `analyze --onsets onsets.csv --resolution-ms 4.17`;
  the frame quantisation is reported.

### Trials

- Warm up for 2 minutes with video before each run.
- At least 200 transitions per run and 3 runs per configuration, on different
  sessions. Report each run, then the pooled distribution.
- Alternate configurations, or randomise their order. Never run all of one
  configuration first.
- Configurations, at minimum:
  - smoothing off;
  - the deployed smoothing settings;
  - with the `ddp` and the `wled` driver, if both are used.
- Record `capture stats` and `LED output stats` for the same minutes (A5/A9)
  and attach the `stats-align.py` output. A run with discards, driver misses,
  failed updates or socket rejections is reported with them.

### Uncertainty and limits

- **Recorder resolution:** 1/sample rate per onset, improved by interpolation.
  It's negligible at 10 kHz.
- **PWM:** up to one PWM period per LED onset. Measure the PWM frequency from
  the raw trace and state it.
- **Thresholds:** the 50% level assumes the sensors are linear over the range
  used. Keep sensor gain, exposure and ambient light fixed within a series.
- **TV:** for M2, the `L_TV` measurement's own uncertainty.
- **Clocks:** one recorder, so none needed. Don't compute latency by matching
  optical times to HyperHDR log times: journal timestamps are another clock with
  1 µs–1 s resolution, and `nowMicro()` has an unknown offset.
- **Sources of spread** that this method measures but can't separate:
  - source frame pacing;
  - receiver buffering;
  - decode;
  - LED refresh phase (up to one refresh period: 16.7 ms at 60 Hz);
  - network;
  - WLED's own frame timing;
  - LED driver chip latency.

### Metadata to record with every run

- **Builds:** HyperHDR commit and package, kernel, WLED version.
- **Capture:**
  - source mode and HDR state;
  - source timing vs configured capture format (A2/A4);
  - RGB range setting;
  - quarter mode and decimation;
  - tone mapping.
- **LED output:**
  - LED driver (`ddp` or `wled`), LED count, refresh rate;
  - smoothing type, time and update frequency;
  - WLED's realtime settings and `leds.fps`;
  - network path (wired or Wi-Fi).
- **Display:** TV model, picture mode and game mode; Vertex2 configuration.
- **Measurement setup:**
  - sensor positions;
  - recorder and sample rate, or camera and frame rate;
  - PWM frequency observed;
  - ambient light.
- **Session:** temperatures and CPU governor before and after; date and time.

## 4. Throughput (#12, #9)

The benchmark ([benchmark plan](rk3588-benchmark-plan.md)) measures decoder cost
on synthetic frames. It isn't throughput from the receiver. On the board, report
the following, each over the same 5-minute window:

| Measurement | Source | Notes |
|---|---|---|
| Source frame rate | `QUERY_DV_TIMINGS` | — |
| Kernel delivery rate | B1, HyperHDR stopped | — |
| Captured and accepted rates, discards by cause | `capture stats` | — |
| Decoded rate and accept → decoded percentiles | `capture stats` | — |
| LED update rate per device | `LED output stats` | Labelled as LED updates |
| Packets per update | `LED output stats` | — |

Never divide one row by another across stages as if they were one pipeline.
