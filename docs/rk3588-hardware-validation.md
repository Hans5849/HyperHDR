# Orange Pi 5 Plus native HDMI capture: operator guide

This guide covers building, installing and validating HyperHDR's native HDMI
receiver support on an Orange Pi 5 Plus (RK3588) running a mainline kernel.
It separates what has been confirmed on the board from what still needs a
connected HDMI source.

## Status

| Area | Status | Evidence |
| --- | --- | --- |
| Native build on Debian 13 ARM64 | Confirmed | Operator build of `cca3898`: GCC 14.2.0, CMake 3.31.6, Qt 6.8.2, Ninja, `ENABLE_V4L2=ON` |
| Local `.deb` install through APT | Confirmed | All dependencies resolved; `hyperhdr@ryan.service` enabled and active |
| Hardware-free regression tests on the board | Confirmed | V4L2 capture test and BGR24 decoder test passed |
| Kernel-level capture (`v4l2-ctl`, outside HyperHDR) | Confirmed earlier | Armbian 26.8.3: HDMI output-to-input loopback, 120 BGR3 1080p frames, ~16.67 ms intervals, no gaps or buffer errors ([mainline review](rk3588-mainline-review.md)) |
| Receiver discovery in HyperHDR | Confirmed | `snps_hdmirx` found at `/dev/video1` (seen earlier as `/dev/video2` and `/dev/video5`); Rockchip non-capture nodes rejected; receiver initialized |
| No-source behaviour at `cca3898` | Confirmed | Streaming started, no frames, five-second timeout, delayed retries |
| No-source behaviour after this update (paused wait, see below) | Not yet run on the board | Covered by simulated tests and CI only |
| HDMI frames, image correctness, preview, WLED/DDP output, CPU/RSS | **Not validated** | No HDMI source has been connected yet |

The loopback run proves the kernel driver delivers frames. It does not prove
HyperHDR's image contents, preview, colours or recovery. Nothing in this guide
implies that real frame capture inside HyperHDR, live image correctness or
WLED output works. Those checks are listed under [Pending hardware validation](#pending-hardware-validation).

## Confirmed environment

| Item | Value |
| --- | --- |
| Board | Orange Pi 5 Plus, Rockchip RK3588, 4 GB RAM |
| OS | Armbian 26.11.0, Debian 13 (Trixie), arm64 (26.8.3 at the time of the loopback test) |
| Kernel | `6.18.44-current-rockchip64` (mainline, not the Rockchip vendor kernel) |
| Toolchain | GCC 14.2.0, CMake 3.31.6, Qt 6.8.2, Ninja |
| Service | `hyperhdr@ryan.service` (systemd template, one instance per user) |

## Receiver facts

| Property | Value on the mainline kernel |
| --- | --- |
| Driver | `snps_hdmirx` (the vendor kernel's equivalent is `rk_hdmirx`; HyperHDR depends on neither name) |
| Bus | `platform:fdee0000.hdmi_receiver` |
| Node | `/dev/videoN`, **assigned dynamically and not stable across boots** |
| API | `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE` |
| Pixel format | `BGR3` (BGR24) |
| Memory planes | 1 |
| `power_present` | Read-only `V4L2_CID_DV_RX_POWER_PRESENT`; `0` when no source supplies +5 V |

HyperHDR classifies every `/dev/video*` node by its reported capabilities. Codec,
RGA and other memory-to-memory nodes, metadata nodes and output-only nodes are
skipped. A one-plane MPLANE capture node is accepted and BGR24 is preferred when
the receiver enumerates no frame sizes. Formats that need several memory planes
are not offered.

### Select the receiver by identity

An unambiguous MPLANE receiver is listed in the HyperHDR device list as
`v4l2:snps_hdmirx@platform:fdee0000.hdmi_receiver` (driver plus bus, see
[mainline review](rk3588-mainline-review.md)). **Select and save that entry.** Its
current `/dev/videoN` is resolved on every start. If that identity is missing,
HyperHDR waits for it and does not switch to another camera.

Older settings keep working. A current `snps_hdmirx (video1)` label or a raw
`/dev/videoN` path resolves to the identity. A label saved before renumbering,
for example `snps_hdmirx (video2)` after the receiver moved to `video1`, is
matched by card name when exactly one present device has that name. The log then
shows `Device ... was renumbered; using ...`. With no unique match, HyperHDR falls
back to automatic selection. Re-save the identity entry to avoid relying on this.

## Find the receiver node

Never assume `/dev/video0`. Look the node up by driver name for each session:

```sh
v4l2-ctl --list-devices
RX=$(v4l2-ctl --list-devices | awk '/snps_hdmirx|rk_hdmirx/ {getline; print $1; exit}')
echo "$RX"
v4l2-ctl -d "$RX" --info
v4l2-ctl -d "$RX" -C power_present
v4l2-ctl -d "$RX" --list-formats-ext
v4l2-ctl -d "$RX" --get-fmt-video
v4l2-ctl -d "$RX" --query-dv-timings
```

With no source, `power_present` reads `0` and `--query-dv-timings` fails (normally
`Link has been severed`, ENOLINK). That is expected and does not affect discovery.

## Build natively on Debian 13 ARM64

Docker is not needed for this path or at runtime.

```sh
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build git pkg-config python3 \
  flatbuffers-compiler libflatbuffers-dev libasound2-dev libdbus-1-dev libegl-dev \
  libftdi1-dev libgl-dev libglvnd-dev liblzma-dev libpipewire-0.3-dev libssl-dev \
  libturbojpeg0-dev libusb-1.0-0-dev libzstd-dev patchelf chrpath unzip wget \
  qt6-base-dev qt6-serialport-dev v4l-utils

git clone --recursive --branch claude/great-heisenberg-bgp44k https://github.com/Hans5849/HyperHDR.git
cd HyperHDR
cmake -S . -B build -G Ninja -DPLATFORM=linux -DENABLE_V4L2=ON \
  -DCMAKE_BUILD_TYPE=Release -DUSE_SYSTEM_FLATBUFFERS_LIBS=ON
cmake --build build
(cd build && cpack -G DEB)
```

For an existing checkout use `git fetch origin && git checkout claude/great-heisenberg-bgp44k && git pull && git submodule update --init --recursive`.

The default (non-CI) package name is `HyperHDR-<application version>-Linux-aarch64.deb`,
for example `HyperHDR-24.0.0-beta.0-Linux-aarch64.deb`. Its control `Version` is
the Debian form of the same version, `24.0.0~beta.0`. `~` makes a prerelease
sort before the final `24.0.0`. Before this change the field read `24.0.0`.

## Hardware-free checks

These run on the board or any Linux host and need no capture device:

```sh
cmake -S tests/v4l2 -B build-v4l2-tests && cmake --build build-v4l2-tests
ctest --test-dir build-v4l2-tests --output-on-failure

cmake -S tests/codec_test -B build-codec-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-codec-tests --target DecoderTest --parallel 4
ctest --test-dir build-codec-tests --output-on-failure

cmake -S tests/cli -B build-cli-tests && cmake --build build-cli-tests
ctest --test-dir build-cli-tests --output-on-failure

cmake -P tests/packaging/debian-version-test.cmake
bash tests/packaging/check-deb.sh build/HyperHDR-*-Linux-aarch64.deb --arch arm64
```

After installation, `tests/cli/hyperhdr-cli-smoke.sh /usr/bin/hyperhdr` checks
`--help`, `--version`, missing `--userdata` values and conflicting log options
without starting the daemon. Stop the service first, because the script refuses
to run while another `hyperhdr` process exists. `--daemon` additionally starts
the daemon briefly with `--debug` and `--verbose` and a throw-away `--userdata`
directory.

## Install and run the service

```sh
svc_user=$(id -un)
sudo systemctl stop "hyperhdr@$svc_user"
dpkg-deb -f build/HyperHDR-*-Linux-aarch64.deb Package Version Architecture
sudo apt install --allow-downgrades ./build/HyperHDR-*-Linux-aarch64.deb
sudo systemctl restart "hyperhdr@$svc_user"
systemctl status "hyperhdr@$svc_user"
journalctl -fu "hyperhdr@$svc_user"
```

`--allow-downgrades` is needed **once**. The earlier development package declared
`Version: 24.0.0`, and `24.0.0~beta.0` correctly sorts below it. After that,
newer prereleases and the final release upgrade normally. Reinstalling a rebuild
with an unchanged version needs `sudo apt install --reinstall ./...deb`.
`hyperhdr --version` shows the build's commit ID.

The service user needs `video` group membership (`sudo usermod -aG video "$svc_user"`,
then restart the service). Only one process may own the receiver; check with
`fuser "$RX"`.

## No-signal operation

### What changed

For receivers that report their input state, HyperHDR no longer starts streaming
without a source. The input state comes from generic V4L2 interfaces only:

1. `power_present` (`V4L2_CID_DV_RX_POWER_PRESENT`): `0` means no source.
2. `VIDIOC_QUERY_DV_TIMINGS`: `ENOLINK` means no signal, `ENOLCK` means not
   locked, `ERANGE` means out of range.

When either reports no input, buffers are released, streaming is not started, and
one non-streaming handle to the receiver is re-probed every 2 seconds. A V4L2
source-change event triggers an immediate re-probe. When the receiver reports a
signal, or stops reporting a state, HyperHDR runs normal discovery and starts
capture. If the node disappears, it falls back to full rediscovery.

Devices that implement neither interface, which includes UVC USB grabbers, are
unaffected. They keep the previous path: stream, five-second frame timeout, then
retries backing off from 3 s up to 30 s.

### Expected log lines

Without a cable (not yet observed on the board, so record the actual output):

```
no signal: no source detected (power_present=0) on /dev/video1; capture paused, checking every 2000 ms
```

The line above is logged once per outage. Repeats go to debug level. After a
source is connected:

```
Input signal on /dev/video1 (detected 1920x1080p); starting capture
Capture API: multiplanar
Input signal: detected 1920x1080p
Video pixel format is set to: BGR24 (limited range)
Started
```

### Failure categories

| Log prefix | Meaning | Next step |
| --- | --- | --- |
| `no signal:` | Receiver reports no source or no lock | Paused wait, 2 s probes, no buffers allocated |
| `timeout:` | Streaming, but no complete frame for 5 s while the receiver does not report a missing signal | Restart with 3 s to 30 s backoff |
| `initialization failed:` | Discovery, open, format negotiation or buffer setup failed | Restart with backoff |
| `streaming failed:` | STREAMON, QBUF or DQBUF failure, invalid payloads, repeated short frames | Restart with backoff |
| `source changed:` | Source-change event while streaming | Fast restart (500 ms) |

Each category is logged as a warning once per outage (as info for `no signal` and
`source changed`) and at debug level while it repeats. `Capture recovered after N
failed attempt(s)` marks the first good frame. Explicitly disabling capture
cancels both pending retries and the signal wait.

## Pending hardware validation

None of the following has been run. Do not mark any item as passed without
captured evidence (logs, screenshots, measurements).

1. Real signal detection and format negotiation: logs show the detected timing,
   BGR24, stride, one plane and quantization.
2. 1920x1080 at about 60 fps (`v4l2-ctl -d "$RX" --stream-mmap=4 --stream-count=600 --stream-to=/dev/null`
   with HyperHDR stopped, then within HyperHDR).
3. Sustained capture for at least 30 minutes.
4. BGR channel order and top-down orientation, using a pattern with labeled
   corners and separate red, green and blue patches.
5. Preview correctness, including limited-range black 16 and white 235 expanding
   to 0 and 255 with no double expansion, quarter-frame, crop and tone mapping
   tested one at a time. Record the startup line
   `RGB range evidence: format quantization ..., receiver RGB range control ...`
   for each source and any `RGB range mismatch:` warning (#10). Decoding follows
   the format quantization only; a mismatch is evidence, not an automatic fix.
6. Reconnection and signal loss: unplug and replug, power the source off and on,
   change the timing and back, ten times each, plus a start with the source
   absent. Expect `no signal` followed by `Input signal ... starting capture`,
   with no stale preview.
7. CPU and memory: idle CPU while waiting for a signal, plus stable RSS, open fd
   count and mapping counts (`/proc/PID/fd`, `/proc/PID/maps`) before and after
   the recovery loops.
8. WLED output and colour: DDP to the controller on UDP 4048, solid colours first,
   then video. Check every border LED, channel order and brightness. A correct
   preview alone does not validate DDP.

Repeat 6 with a USB grabber to confirm the single-planar path is unchanged.

## Collect evidence and troubleshoot

Start with `scripts/diagnostics/hyperhdr-doctor.sh` ([diagnostics](diagnostics.md)).
It checks the package, service, user data, ports, groups and receiver state, and
gives a fix for each problem. Then run the evidence collector on the board at the
start and end of every session,
and attach its archive to the relevant issue (#8, #10, #11, #13):

```sh
scripts/rk3588/collect-evidence.sh                 # read-only, safe while HyperHDR runs
sudo systemctl stop "hyperhdr@$svc_user"
scripts/rk3588/collect-evidence.sh --stream 600 --expbuf   # kernel stream + DMA-BUF export probe
sudo systemctl start "hyperhdr@$svc_user"
```

It records:
- OS, Armbian and kernel versions, CPU governors and temperatures
- the installed package version and the binary's SHA-256, plus `hyperhdr --version`
- the service state
- every video node's capabilities
- the receiver's formats, DV timings, controls (including `power_present` and the RGB-range control) and which processes hold it
- the last 30 minutes of HyperHDR and kernel video logs

It prints a one-screen summary and writes `rk3588-evidence-<UTC time>.tar.gz`.

The default run changes nothing: no service, package, device-setting or streaming
changes. `--stream N` and `--expbuf` open the receiver, so stop HyperHDR first;
otherwise they report the node as busy. `--expbuf` compiles
`scripts/rk3588/v4l2-expbuf-probe.c` with `cc`. It requests two MMAP buffers, tries
`VIDIOC_EXPBUF` and frees them again without streaming.

IPv4/IPv6/MAC addresses, the hostname and the user name are redacted, and the
HyperHDR database is never read. Still review the archive before attaching it, and
do not share configuration exports that contain network credentials. A stream
result is kernel-level evidence only, not HyperHDR preview or WLED evidence.

- Discovery: check `video` group membership, the node's capability flags and for
  competing processes. Skipped nodes are logged at debug level with a reason.
- `initialization failed: format negotiation`: record the returned format and
  errno. Do not force NV12 to work around a BGR3 failure.
- `streaming failed`: DQBUF, QBUF or STREAMON errors and invalid payloads. Partial
  setups are cleaned up before every retry.
- Correct preview but wrong LEDs: check WLED realtime state, address, LED count,
  channel order and UDP 4048 routing separately.

## Self-hosted board runner

`.github/workflows/rk3588-board.yml` runs on a dedicated, repository-scoped runner
on the Orange Pi 5 Plus. It has labels `self-hosted, Linux, ARM64, rk3588,
orange-pi-5-plus, hyperhdr-hil`, runs from `master` only, and goes through the
`rk3588-hil` environment.

Start it from **Actions → RK3588 board checks → Run workflow**. Each run begins
with `scripts/rk3588/board-preflight.sh`, which fails the run unless all of these
hold:
- the board is an Orange Pi 5 Plus with 4x A55 + 4x A76
- the runner isn't root and has no passwordless sudo
- for `probe`, the receiver is accessible
- for EXPBUF, no HyperHDR is running

Stages:
- **`tests`:** sanitizer suites natively on ARM64, and the BGR24 benchmark on the
  A76 and A55 cores.
- **`probe`:** also the diagnostics doctor (#27) and the evidence collector, plus
  the optional EXPBUF probe.

Provisioning, trust boundaries, the disruption policy and runner removal are in
[rk3588-board-runner.md](rk3588-board-runner.md).

## Roll back

Stop the service. Install the saved official arm64 package with
`sudo apt install --allow-downgrades ./EXACT-STABLE.deb`, restore the settings
backup (`~/.hyperhdr` of the service user, including its database) with its
original ownership, and start the service again. This branch requires no reboot
and no kernel change.

## Optional: Docker package build

`build.sh` can produce packages in the project's container images (for example
`PLATFORM=linux DISTRO_NAME=debian DISTRO_VERSION=trixie ARCHITECTURE=arm64 bash ./build.sh`
on a native arm64 host). This is a build-time convenience only. The native build
above is the confirmed path. CI builds and checks the Debian Trixie amd64 and
arm64 packages on every push.
