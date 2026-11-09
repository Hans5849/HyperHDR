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
| Receiver discovery | Confirmed | `snps_hdmirx` found at `/dev/video1` (seen earlier as `/dev/video2` and `/dev/video5`); Rockchip non-capture nodes rejected; receiver initialized |
| No-source behaviour at `cca3898` | Confirmed | Streaming started, no frames, five-second timeout, delayed retries |
| No-source behaviour after this update (paused wait, see below) | Not yet run on the board | Covered by simulated tests and CI only |
| HDMI frames, image correctness, preview, WLED/DDP output, CPU/RSS | **Not validated** | No HDMI source has been connected yet |

Nothing in this guide implies that real frame capture, live image correctness or
WLED output works. Those checks are listed under [Pending hardware validation](#pending-hardware-validation).

## Confirmed environment

| Item | Value |
| --- | --- |
| Board | Orange Pi 5 Plus, Rockchip RK3588, 4 GB RAM |
| OS | Armbian 26.11.0, Debian 13 (Trixie), arm64 |
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

A device saved in the configuration is stored as `<card name> (videoN)`. If the
node number changes, HyperHDR now selects the device with the same card name
when exactly one is present, and logs `Device ... was renumbered; using ...`.
If no device, or more than one device, has that name, it falls back to automatic
selection as before.

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
   tested one at a time.
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

Save the package version (`dpkg-query -W hyperhdr`), commit, kernel, the
`v4l2-ctl` output above, `journalctl -u "hyperhdr@$svc_user" --since '-30 minutes'`
and `journalctl -k --since '-10 minutes'`. Do not share configuration exports that
contain network credentials.

- Discovery: check `video` group membership, the node's capability flags and for
  competing processes. Skipped nodes are logged at debug level with a reason.
- `initialization failed: format negotiation`: record the returned format and
  errno. Do not force NV12 to work around a BGR3 failure.
- `streaming failed`: DQBUF, QBUF or STREAMON errors and invalid payloads. Partial
  setups are cleaned up before every retry.
- Correct preview but wrong LEDs: check WLED realtime state, address, LED count,
  channel order and UDP 4048 routing separately.

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
