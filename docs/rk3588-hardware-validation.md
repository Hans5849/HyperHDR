# Orange Pi 5 Plus development-build validation

These are operator instructions. No physical Orange Pi, HDMI source, WLED device,
package installation or reboot was accessed by the development environment.
The prior and mainline 1080p60 BGR3 captures are user-supplied kernel-level
evidence; this branch still needs all application-level hardware checks below.
For the current Armbian 26.8.3 / Debian 13 Trixie mainline setup, start with
[rk3588-mainline-review.md](rk3588-mainline-review.md): `snps_hdmirx`, bus
`platform:fdee0000.hdmi_receiver`, currently `/dev/video2`, kernel
`6.18.44-current-rockchip64`. Save its stable driver/bus identity in HyperHDR.
The Bookworm/vendor setup below records the original development target; it is
not a requirement for mainline capture. Use the Trixie ARM64 image/target for the
new verified OS. No kernel change or vendor driver is required.

## Preserve a rollback path first

Record `uname -a`, `cat /etc/os-release`, `dpkg --print-architecture`,
`dpkg-query -W hyperhdr`, the running service user, and installed package source.
Save the exact official stable **arm64** .deb and verify its trusted release
checksum. Keep it locally before replacing anything. Export HyperHDR settings
from the web UI and back up the service user's entire `~/.hyperhdr` directory
while HyperHDR is stopped (including its database). Keep WLED configuration and
an image of the working Armbian installation if available. A development version
may migrate settings; rollback must restore the pre-development backup too.
Keep the currently working kernel; do not switch to a vendor kernel for this test.

## Build and package

The repository's BUILDING.md supports Bookworm ARM64 through HyperDockerBuilder.
On a native ARM64 Docker build host, or an x86 host with working qemu-user-static
and registered binfmt_misc, from a clean branch checkout:

```sh
git clone --recursive --branch feature/rk3588-hdmirx https://github.com/Hans5849/HyperHDR.git
cd HyperHDR
git submodule update --init --recursive
# Only if the pinned CMock submodule was skipped by its update=none setting:
git -C external/core_mqtt -c submodule.test/unit-test/CMock.update=checkout submodule update --init --recursive
PLATFORM=linux DISTRO_NAME=debian DISTRO_VERSION=bookworm ARCHITECTURE=arm64 \
  BUILD_ARCHIVES=false USE_CCACHE=false bash ./build.sh
```

`build.sh` uses the Linux default V4L2 option. For an explicit V4L2-enabled package
build, use the same supported toolchain image directly, from the checkout root:

```sh
mkdir -p deploy
docker pull ghcr.io/awawa-dev/arm64/debian:bookworm
docker run --rm --platform linux/arm64 \
  --mount "type=bind,src=$PWD,dst=/source,readonly" \
  --mount "type=bind,src=$PWD/deploy,dst=/deploy" \
  --entrypoint /bin/bash ghcr.io/awawa-dev/arm64/debian:bookworm -lc '
    set -e
    mkdir /hyperhdr
    tar -C /source --exclude=./build --exclude=./deploy --exclude=./.ccache --exclude=./.git -cf - . | tar -C /hyperhdr -xf -
    cmake -S /hyperhdr -B /hyperhdr/build -DPLATFORM=linux -DENABLE_V4L2=ON \
      -DCMAKE_BUILD_TYPE=Release -DDEBIAN_NAME_TAG=bookworm -DBUILD_ARCHIVES=OFF \
      -DENABLE_DEPENDENCY_PACKAGING=ON -DUSE_SYSTEM_FLATBUFFERS_LIBS=ON
    cmake --build /hyperhdr/build --parallel 4
    grep "^ENABLE_V4L2:BOOL=ON" /hyperhdr/build/CMakeCache.txt
    (cd /hyperhdr/build && cpack -G DEB)
    cp /hyperhdr/build/*.deb /deploy/
  '
```

This is emulated native compilation on x86, not a standalone cross-compiler.
The development cloud pulled the Bookworm ARM64 image, but the supported script
stopped at its QEMU/binfmt check. No ARM64 executable or package was produced.
The direct command is documented for reproduction and was not executed to
completion here. Record the image digest used when reproducing. The remote branch
must contain these commits first; if publication was blocked, apply the delivered
patches to b1451f2 on a local `feature/rk3588-hdmirx` branch instead.

For native compilation, BUILDING.md lists these Debian/Ubuntu dependencies:

```sh
sudo apt-get update
sudo apt-get install build-essential cmake flatbuffers-compiler git libasound2-dev \
  libdbus-1-dev libegl-dev libflatbuffers-dev libftdi1-dev libgl-dev libglvnd-dev \
  liblzma-dev libpipewire-0.3-dev libssl-dev libturbojpeg0-dev libusb-1.0-0-dev \
  libzstd-dev ninja-build patchelf pkg-config python3 qt6-serialport-dev \
  qt6-base-dev unzip wget chrpath v4l-utils
cmake -S . -B build-native -DPLATFORM=linux -DENABLE_V4L2=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUSE_SYSTEM_FLATBUFFERS_LIBS=ON
cmake --build build-native --parallel 4
(cd build-native && cpack -G DEB)
```

Use the Docker toolchain if distribution compiler/library versions cannot build
the current C++20 project. A Bookworm-compatible package should be built in the
Bookworm image, not copied from the Trixie amd64 development environment.

Hardware-free tests (also runnable on a Linux development host):

```sh
cmake -S tests/v4l2 -B build-v4l2-tests
cmake --build build-v4l2-tests
ctest --test-dir build-v4l2-tests --output-on-failure
cmake -S tests/codec_test -B build-codec-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-codec-tests --target DecoderTest --parallel 4
ctest --test-dir build-codec-tests --output-on-failure
```

The codec CTest case needs no external fixtures. The longer legacy DecoderTest
comparison additionally needs the seven generated frames and extracted LUT;
see `tests/v4l2/README.md`.

## Install on the board

Copy only the new arm64 .deb to the Orange Pi. Substitute exact filenames below.
Validate `dpkg-deb -f ./HyperHDR-development.deb Architecture` prints `arm64` and
record its SHA-256 and version. Substitute the actual service account where it
differs from the logged-in user.

```sh
svc_user=$(id -un)
sudo systemctl stop "hyperhdr@$svc_user"
sudo apt install ./HyperHDR-development.deb
sudo systemctl stop "hyperhdr@$svc_user"
sudo usermod -aG video "$svc_user"
ls -l /dev/video0
id "$svc_user"
```

The package's postinst may start the service on a console OS. Stop it again
before standalone capture checks. Restarting the service refreshes supplementary
groups; an interactive shell needs a new login. Use video-group access, not
world-writable device permissions. Verify a single instance owns the device with
`fuser /dev/video0`. DDP needs network access, not root.

## Confirm the source and discover the device

With HyperHDR stopped and the Vertex2 secondary output connected:

```sh
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --all
v4l2-ctl -d /dev/video0 --list-formats-ext
v4l2-ctl -d /dev/video0 --query-dv-timings
v4l2-ctl -d /dev/video0 --get-fmt-video
v4l2-ctl -d /dev/video0 --stream-mmap=4 --stream-count=600 --stream-to=/dev/null
```

For the original vendor setup expect rk_hdmirx, VIDEO_CAPTURE_MPLANE and STREAMING, BGR3, 1920x1080, one plane,
bytesperline 5760, sizeimage 6220800 and limited quantization for the verified
setup. Record actual values. The mainline run enumerated only BGR3; its quantization was not supplied.
NV24/NV16 are unsupported by this decoder. Advertised
NV12 is not proof of successful negotiation; its prior TRY_FMT failed with EINVAL.
Do not use FFmpeg's standard V4L2 input as the compatibility criterion.

Start the development build in foreground for initial diagnosis:

```sh
hyperhdr --help
hyperhdr -d 2>&1 | tee hyperhdr-native-hdmi.log
```

Use the same account/configuration as the service, and ensure the service is
stopped. Alternatively `sudo systemctl start "hyperhdr@$svc_user"` and use
`journalctl -fu "hyperhdr@$svc_user"`. Open `http://BOARD_ADDRESS:8090` (or the
actual port printed by HyperHDR). Enable video capture, choose the discovered
rk_hdmirx entry, BGR24 (`bgr24`), current/automatic resolution and 60 fps if
reported; use automatic/unknown frame rate if the receiver has no frame-interval
API. Begin with crop, software decimation, tone mapping and smoothing disabled.

Logs must show `Capture API: multiplanar`, returned resolution, stride, one plane,
quantization, BGR24 limited range and successful start. UI discovery must work
without hardcoding /dev/video0. Where practical, test another node assignment.
Multi-memory-plane layouts are explicitly rejected before worker dispatch.

## Preview, WLED and recovery acceptance

Use live preview and a source with labeled corners, row numbers and independent
red/green/blue patches. Verify top-down orientation, channel order and no row
skew. Test limited black 16, white 235 and gray ramps: these should expand to
0/255 with no double expansion elsewhere. Then test the quarter-frame setting,
cropping and optional tone mapping independently. Save preview screenshots.

Configure the LED controller as DDP, host = the ESP32/WLED address, UDP port 4048,
correct LED count/layout and RGB ordering. Enable WLED realtime/DDP reception
and confirm the network path. First send solid colors from HyperHDR to isolate
LED/network configuration, then switch to HDMI video. Confirm all border LEDs,
color matching and response. A working preview alone does not validate DDP.
Optional `tcpdump -ni INTERFACE udp port 4048` establishes packet emission, not
physical LED correctness. Measure optical response using the benchmark plan.

Run at least 30 minutes. Unplug/replug HDMI, switch off/on the source, change
1080p60 to another supported timing and back, and repeat each ten times. Also
start with the source absent and then connect it. Expect source-change/timeout
logs, teardown and retries at three-second intervals, followed by fresh frames.
No old mappings or worker-owned buffers should survive teardown. Look for bounded
CPU while absent, stable RSS/fd/map counts, no stale preview, and correct LEDs
after recovery. Explicitly disabling capture must cancel retries. Validate a
single-planar USB capture device and repeated enable/disable as regression checks.

## Collect evidence and troubleshoot

Save the package/commit/kernel versions, CMakeCache, device capabilities/formats,
application debug log and `journalctl -k --since '-10 minutes'`. Collect
`journalctl -u "hyperhdr@$svc_user" --since '-30 minutes'`, process RSS,
`ls /proc/PID/fd | wc -l`, and mapping counts before/after recovery loops. Avoid
sharing configuration exports containing network credentials.

- Discovery: verify video-group permissions, node capability flags and no competing
  process. Nodes without streaming capture are skipped with a discovery reason.
- Format negotiation: capture the returned format and errno; EINVAL can indicate
  unsupported BGR3/timing/plane layout. Do not force NV12 to work around it.
- Buffer setup: QUERYBUF/mmap/REQBUFS errors identify allocation problems. Check
  vendor kernel logs and available memory. Partial setup is cleaned before retry.
- Streaming: DQBUF/QBUF/STREAMON errors, invalid payloads, source changes or five
  seconds without a good frame trigger teardown and delayed restart. A failed
  cleanup operation is logged and the fd is closed. EAGAIN wakeups are throttled.
- Correct preview but wrong LEDs: verify WLED realtime state, address, LED count,
  channel order, UDP 4048 routing, brightness and smoothing separately.
- Persistent no-source or timing recovery failure: save diagnostics and disable
  capture while investigating. Do not infer that the kernel supports every timing.

## Roll back

Stop the development service. Install the previously saved exact official stable
arm64 package using `sudo apt install --allow-downgrades ./EXACT-STABLE.deb`.
Stop any instance started by postinst, restore the pre-development settings
backup with its original ownership, and start the original service account.
Verify stable version, UI and the previously working capture configuration.
Keep development logs/settings separately. No reboot is required by this branch;
any later kernel maintenance is a separate operator decision.
