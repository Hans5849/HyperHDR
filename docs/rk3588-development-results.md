# RK3588 native HDMI development results

Development date: 2026-11-04. Base: b1451f2868072c5b8eda0ae61c0fcf33ecc1bfc5
in Hans5849/HyperHDR. Original branch: `feature/rk3588-hdmirx`, merged to
`master` through PR #1. Later work lands on `master` through pull requests.

The sections below are dated. The newest status is first; older sections are kept
as history, and statements they make that are no longer true are marked
superseded.

## Current status (2026-11-15)

| Item | Status | Evidence |
| --- | --- | --- |
| Original implementation | **Merged** (PR #1, 2026-11-04) | |
| ARM64 packages | **Produced by hosted CI** for Debian Bookworm and Trixie, and also Bullseye and Forky (`HyperHDR-24.0.0~bookworm~beta0-aarch64.deb`, `…~trixie~beta0-aarch64.deb`) | `master` run [43719047798](https://github.com/Hans5849/HyperHDR/actions/runs/43719047798) |
| Hardware-free capture tests in hosted CI | **Integrated** on x86_64 and ARM64 under ASan/UBSan: V4L2 capture and recovery (including #33), the independent BGR24 reference suite (#31), the real `FrameDecoder` BGR24 dispatch, CLI, packaging, evidence collector, diagnostics and board-workflow policy tests | `CaptureTests` jobs in every `push-pull.yml` run |
| Native build and install on the Orange Pi | **Done** (2026-11-09) | See the update below |
| Board workflow (`RK3588 board checks`) | **Needs a successful rerun.** The first run passed all sanitizer suites natively on the board but failed the benchmark step on the executable path (`./build-codec/DecoderTest` instead of `build-codec/bin/DecoderTest`). The path is fixed on `master`; the workflow hasn't been rerun since | Run [43716032809](https://github.com/Hans5849/HyperHDR/actions/runs/43716032809) |
| Hardware validation | **Separate from all of the above, and still pending**: HDMI frames inside HyperHDR, image correctness, preview, reconnect behaviour, WLED/DDP output, CPU and memory under load | [Hardware validation](rk3588-hardware-validation.md), #8 |

Compilation and passing hosted CI show that the code builds and that its
hardware-free tests pass on both architectures. They don't show that capture works
on the board. What each frame count and timing measures (captured, accepted,
decoded, LED output) is defined in [frame stages](rk3588-frame-stages.md). No
end-to-end or WLED output latency has been measured.

## 2026-11-09 update: first board results and follow-up fixes

An operator built `cca3898` natively on the Orange Pi 5 Plus (Armbian 26.11.0,
Debian 13 arm64, mainline kernel 6.18.44, GCC 14.2.0, CMake 3.31.6, Qt 6.8.2,
Ninja). The package installed through APT, `hyperhdr@ryan.service` runs, and both
hardware-free tests passed on the board. The receiver is the mainline
`snps_hdmirx` driver (not `rk_hdmirx`) at a dynamic `/dev/videoN`, MPLANE, BGR3,
one memory plane, with a read-only `power_present` control. It was discovered and
initialized correctly. With no HDMI source connected, streaming started and then
cycled through the five-second timeout. **No HDMI frames, preview or WLED output
have been validated.**

Follow-up changes:

- Command line: flags no longer trigger `QCommandLineParser: option not expecting
  values` warnings. Conflicting `--silent`, `--verbose` and `--debug` now exit 1
  instead of 0. Parser unit tests and an installed-binary smoke test were added.
- No signal: receivers that report `power_present=0` or a DV-timings
  `ENOLINK`/`ENOLCK`/`ERANGE` are no longer streamed. Capture pauses with no
  buffers allocated, re-probes every 2 s and on source-change events, and starts
  when a signal appears. Devices without these interfaces (USB) are unchanged.
  Logs distinguish no signal, timeout, initialization, streaming and source-change
  failures.
- Discovery: memory-to-memory nodes are rejected explicitly. A saved device is
  re-found by its card name after renumbering. Regression tests cover node
  reordering, Rockchip codec/RGA nodes, MPLANE, one-plane and multi-plane
  formats, and discovery without a signal.
- Packaging: the default `.deb` now declares `Version: 24.0.0~beta.0` instead of
  `24.0.0`. A `dpkg-deb` check runs in CI for every Debian/Ubuntu package.

See `rk3588-hardware-validation.md` for the operator procedure and pending checks.

## 2026-11-15 manual Cortex-A76 decoder timings (not a baseline)

Measured by hand on the Orange Pi 5 Plus (Armbian, Debian 13, kernel 6.18.44) on
CPU 4 (Cortex-A76) with `taskset -c 4 build-codec/bin/DecoderTest --bgr24-bench`:
1920x1080 BGR24, 200 timed runs per row. Decoder time only, not HDMI-to-LED
latency, and the input is the benchmark's synthetic frame, not the receiver's
current signal.

| Range | Output | LUT | Median | P95 |
|---|---|---|---:|---:|
| Full | Full | No | 0.618 ms | 0.684 ms |
| Full | Full | Yes | 52.894 ms | 67.396 ms |
| Full | Quarter | No | 0.842 ms | 0.886 ms |
| Full | Quarter | Yes | 13.353 ms | 16.855 ms |
| Limited | Full | No | 4.932 ms | 5.034 ms |
| Limited | Full | Yes | 63.081 ms | 79.167 ms |
| Limited | Quarter | No | 1.314 ms | 1.370 ms |
| Limited | Quarter | Yes | 16.182 ms | 18.376 ms |

What they show: LUT decoding costs one to two orders of magnitude more than
non-LUT decoding at this size. What they don't establish: a reproducible
baseline. The board is shared with another runner, load average, governor and
temperature weren't recorded for these runs, and there is no Cortex-A55 result
yet. The `RK3588 board checks` workflow now records both cores with that context
(`bgr24-bench-a76.txt`, `bgr24-bench-a55.txt`); the A55 result is still to be
captured from an Actions run on the board.

The first workflow run on the board passed every sanitizer suite natively but
failed the benchmark with exit status 127: it ran `./build-codec/DecoderTest`,
while CMake writes the executable to `build-codec/bin/DecoderTest` (confirmed by the
hosted build log: `Linking CXX executable bin/DecoderTest`). Fixed in PR #30 and
guarded by `tests/ci/board-workflow-policy-test.py`. **A successful rerun is still
needed.**

## Original implementation (2026-11-04)

No merge, upstream PR, release workflow change, release or physical-board action
was performed in that session.

## Implemented

- Effective node capability detection (DEVICE_CAPS), single-planar preference
  when both APIs exist, and MPLANE format/buffer selection.
- TRY_FMT validation of advertised MPLANE modes, current-timing discovery for
  native receivers with incomplete enumeration, and BGR24 fallback preference.
- MMAP ownership with per-plane descriptors, index-based worker buffer returns,
  data_offset/bytesused bounds checks and no extra full-frame copy.
- Explicit rejection of separate-memory-plane decoder layouts; the generic
  mapper supports and tests their ownership/partial cleanup.
- Native top-down BGR24 decoding, padded stride, crop/quarter output, limited
  RGB expansion and optional LUT, preserving legacy decoder semantics.
- Partial-start cleanup, worker completion generation checks, error recovery,
  source-change handling, timing refresh, five-second no-good-frame watchdog,
  three-second retry and bounded EAGAIN wakeups. Explicit stop cancels retries.
- Hardware-free capture and actual decoder tests plus four requested guides.

## Build and test evidence

| Check | Result and limits |
| --- | --- |
| Original project, explicit ENABLE_V4L2=ON | PASS, fresh invocation of prepared Docker toolchain before application edits |
| Modified project, explicit ENABLE_V4L2=ON | PASS, full native x86_64 RelWithDebInfo Linux application |
| Capture CTest | PASS, 1 registered case / 10 groups, strict -Wall -Wextra -Werror |
| BGR24 decoder CTest | PASS, 1 registered case / 16 combinations using actual dispatch |
| Capture address/undefined/leak sanitizers | PASS inside Docker, no reported errors |
| Existing decoder comparisons | PASS, all 28 combinations, no ERROR or mismatch output |
| Native Debian package | PASS, HyperHDR-24.0.0-beta.0-Linux-x86_64.deb; development Trixie amd64, dependency bundling OFF |
| HTTP/sysinfo JSON-RPC smoke | PASS in isolated Docker daemon service, no hardware |
| Bookworm ARM64 package | BLOCKED before compilation by missing QEMU/binfmt; image pull succeeded. *Superseded: hosted CI now builds ARM64 Bookworm and Trixie packages (see Current status).* |
| HDMI/Orange Pi/WLED | NOT RUN; no attached hardware |

Toolchain: Debian Trixie image, GCC 14.2, C++20, CMake 3.31.6, Qt 6.8.4.
Normal Linux features remained enabled, including V4L2, PipeWire, X11, sound and
networking. This is not an ARM64 compilation or a board-compatibility certificate.

Two initial verification invocations needed correction: the legacy decoder test
had no mounted HOME/LUT and therefore reported errors; it was rerun using the
prepared fixtures and all 28 comparisons passed. Host LeakSanitizer could not run
under the execution sandbox's tracing; the same sanitizer test passed in Docker,
with leak checking enabled. Neither initial attempt is counted as a pass.

Commands and detailed logs are included in the review archive. The new small
suites run independently of physical video devices. The existing root application
build does not register these standalone suites; their documented CTest commands
must be used explicitly.

The ARM64 attempt used the supported command:

```sh
PLATFORM=linux DISTRO_NAME=debian DISTRO_VERSION=bookworm ARCHITECTURE=arm64 \
  BUILD_ARCHIVES=false USE_CCACHE=false bash ./build.sh
```

It pulled `ghcr.io/awawa-dev/arm64/debian:bookworm`, then stopped because the host
has no registered QEMU/binfmt setup. At that time (2026-11-04) no ARM64 target was
compiled and no ARM64 .deb existed. *Superseded: hosted CI now builds them on
native ARM64 runners, and the board built one natively on 2026-11-09.* The validation guide gives the supported script and an explicit
V4L2-enabled Docker/CMake/CPack command for a capable host. No release workflow,
secrets or privileged host emulation changes were added.

## Files changed

Capture implementation:

- include/grabber/linux/v4l2/V4L2Capture.h (new)
- include/grabber/linux/v4l2/V4L2Grabber.h
- sources/grabber/linux/v4l2/V4L2Grabber.cpp
- include/utils/Bgr24.h (new)
- include/utils/PixelFormat.h
- sources/utils/FrameDecoder.cpp

Tests:

- tests/v4l2/CMakeLists.txt, main.cpp, README.md (new)
- tests/codec_test/Bgr24Checks.h (new)
- tests/codec_test/CMakeLists.txt and main.cpp

Documentation:

- docs/rk3588-v4l2-architecture.md
- docs/rk3588-hardware-acceleration.md
- docs/rk3588-benchmark-plan.md
- docs/rk3588-hardware-validation.md
- docs/rk3588-development-results.md

## Remaining review and hardware work

Validate both normal single-planar capture and the native receiver (`snps_hdmirx` on mainline) against real
kernel devices. In particular, confirm TRY_FMT/S_FMT behavior, source-event
subscription, DV timing changes, quantization, top-down preview and repeated
start/stop/disconnect recovery. Timer scheduling and worker teardown were source
reviewed; mocked buffer tests do not establish their behavior against a kernel
HDMI driver. Unsupported separate-plane decoder formats remain an explicit
scope limit. Capture/LED latency and CPU performance remain unmeasured (manual
decoder-only timings are in the 2026-11-15 section above).

Superseded: the package was later built natively on Debian 13 arm64 and
installed on the Orange Pi (see the 2026-11-09 update). HDMI and WLED/DDP
validation is still pending.

Recommend an optional RGA prototype using persistent DMA-BUF imports and bounded
output pools; confirm EXPBUF support and fence/cache semantics first. Compare
against the CPU quarter-frame path including every copy and synchronization cost.
Defer GPU work and use the NPU only for actual neural inference. No accelerator
or speedup is included in this implementation.

## Publication

The GitHub CLI authentication check reported an invalid token, but the managed
Git connection successfully pushed the branch and the connected GitHub tool
created [PR #1](https://github.com/Hans5849/HyperHDR/pull/1), targeting
`Hans5849/HyperHDR:master`. It was merged on 2026-11-04. No upstream PR was
opened.
