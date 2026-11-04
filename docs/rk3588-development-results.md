# RK3588 native HDMI development results

Development date: 2026-11-04. Base: b1451f2868072c5b8eda0ae61c0fcf33ecc1bfc5
in Hans5849/HyperHDR. Local development branch: `feature/rk3588-hdmirx`.
No merge, upstream PR, release workflow change, release or physical-board action
was performed.

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
| Bookworm ARM64 package | BLOCKED before compilation by missing QEMU/binfmt; image pull succeeded |
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
has no registered QEMU/binfmt setup. No ARM64 target was compiled and no ARM64
.deb exists. The validation guide gives the supported script and an explicit
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

Validate both normal single-planar capture and native rk_hdmirx against real
kernel devices. In particular, confirm TRY_FMT/S_FMT behavior, source-event
subscription, DV timing changes, quantization, top-down preview and repeated
start/stop/disconnect recovery. Timer scheduling and worker teardown were source
reviewed; mocked buffer tests do not establish their behavior against a kernel
HDMI driver. Unsupported separate-plane decoder formats remain an explicit
scope limit. Capture/LED latency and CPU performance remain unmeasured.

Build Bookworm ARM64, install manually on the Orange Pi, then validate HDMI and
WLED/DDP with the supplied rollback procedure. No hardware success is claimed.

Recommend an optional RGA prototype using persistent DMA-BUF imports and bounded
output pools; confirm EXPBUF support and fence/cache semantics first. Compare
against the CPU quarter-frame path including every copy and synchronization cost.
Defer GPU work and use the NPU only for actual neural inference. No accelerator
or speedup is included in this implementation.

## Publication

The available GitHub CLI authentication check reported an invalid token. Local
commits and a review archive are available independently of GitHub credentials.
See the accompanying review handoff for the final push result and commit list.
A future draft PR must target `Hans5849/HyperHDR:master`, never the upstream repo.
