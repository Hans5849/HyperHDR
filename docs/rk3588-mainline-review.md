# RK3588 mainline HDMI RX review

## Confirmed hardware evidence supplied by the operator

Orange Pi 5 Plus, RK3588, Armbian 26.8.3 / Debian 13 Trixie, kernel
`6.18.44-current-rockchip64`. The receiver identifies as driver `snps_hdmirx`,
bus `platform:fdee0000.hdmi_receiver`, currently `/dev/video2`. It exposes
VIDEO_CAPTURE_MPLANE and BGR3 with one memory plane: 1920x1080, bytesperline 5760,
sizeimage/payload 6,220,800, MMAP, approximately 60 fps.

An HDMI-output-to-input loopback streamed 120 frames using v4l2-ctl. STREAMON
succeeded; all frames were dequeued at roughly 16.67 ms intervals without buffer
errors or sequence gaps. This confirms kernel capture on this configuration.
It does **not** confirm image contents, HyperHDR preview, colors, recovery, WLED
or optical latency. Quantization was not supplied for this run; do not copy the
older vendor-kernel limited-range assumption into this configuration.

## Assessment of the existing branch

| Area | Existing support and review result |
| --- | --- |
| Capture API | DEVICE_CAPS-aware selection already uses VIDEO_CAPTURE_MPLANE when required, including exactly one memory plane; single-planar preference is unchanged |
| Buffer lifecycle | REQBUFS, QUERYBUF, per-plane MMAP, QBUF, DQBUF, STREAMON/OFF and partial cleanup already implemented; workers return a fresh descriptor by buffer index |
| Pixel format | BGR3 maps to the distinct top-down BGR24 decoder; no NV12 or intermediate full-frame conversion is required |
| Layout | Negotiated stride drives row access; payload/data_offset bounds use actual mapping lengths; fix adds a minimum mapping-size check against negotiated sizeimage |
| Source recovery | Source-change notifications, five-second watchdog and three-second retries already exist; fix makes acquisition failure stop before stale buffer allocation and compares meaningful DV timing fields |
| Discovery | Existing enumeration found nodes generically but persisted a numbered name; fix adds driver+bus selectors for unambiguous MPLANE nodes and preserves legacy selection aliases |
| Mainline API | Linux v6.18 snps_hdmirx exposes the standard ioctls used here; no kernel-name check or Rockchip vendor API/dependency is required |
| Latency | Linux workers read the MMAP address directly, then the decoder writes RGB output; there is no additional input-frame staging copy |

Separate-memory-plane decoder formats remain explicitly unsupported. The mapper
can own multiple planes, but that does not establish multi-plane image decoding.
Other capture backends and color-processing behavior are unchanged.

## Targeted corrections

1. `v4l2:DRIVER@BUS` is the selectable identity for an unambiguous MPLANE node.
   The verified receiver becomes
   `v4l2:snps_hdmirx@platform:fdee0000.hdmi_receiver`. Its current node is resolved
   on every enumeration. A selected identity that disappears waits for that
   receiver instead of automatically selecting an unrelated camera. Old current
   labels and raw paths still resolve; canonical device symlinks also resolve.
   Save the stable selection in HyperHDR: a legacy numbered configuration alone
   cannot identify what used to occupy that number after a process restart.
   Missing driver/bus fields or several capture nodes sharing the same identity
   retain distinct legacy names; an ambiguous identity is never silently merged.
2. Standard QUERY_DV_TIMINGS no-link/unlocked errors now abort initialization
   before S_FMT/REQBUFS and enter the existing delayed retry. Unsupported DV
   timing ioctls on ordinary cameras are tolerated. When signal geometry changes,
   S_DV_TIMINGS runs before format negotiation and buffer allocation. Reserved
   bytes and descriptive flags cannot cause a spurious timing mismatch.
3. Every first-plane QUERYBUF mapping must cover negotiated sizeimage. Mapping
   lengths can exceed that minimum. BGR decoding still uses bytesperline and the
   minimum last-row extent, not an assumed tightly packed frame. Padding is not
   interpreted as pixels. Bounds checks and partial allocation rollback remain.

## Configuration for the verified mainline system

Keep the working kernel and driver. No vendor kernel, kernel switch, RGA or NPU
is required. Follow the dependency list in BUILDING.md; use the Trixie ARM64 build
image for this OS if building packages. The previous Bookworm packaging notes
remain historical results, not a mainline-driver prerequisite.

For a Trixie ARM64 package using the documented Docker build method:

```sh
PLATFORM=linux DISTRO_NAME=debian DISTRO_VERSION=trixie ARCHITECTURE=arm64 \
  BUILD_ARCHIVES=false USE_CCACHE=false bash ./build.sh
```

An x86 build host needs working QEMU/binfmt for this emulated ARM64 build. The
review workspace has not produced a new ARM64 package. For a native application
build, use the dependencies in BUILDING.md and explicitly configure
`cmake -S . -B build -DPLATFORM=linux -DENABLE_V4L2=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo`.

With HyperHDR stopped, enumerate and confirm identity rather than assuming a node:

```sh
v4l2-ctl --list-devices
# Set this from that output; video2 is the operator's current observation only.
capture_device=/dev/video2
v4l2-ctl -d "$capture_device" --all
v4l2-ctl -d "$capture_device" --list-formats-ext
v4l2-ctl -d "$capture_device" --get-fmt-video --query-dv-timings
v4l2-ctl -d "$capture_device" --stream-mmap --stream-count=120 --stream-to=/dev/null --verbose
```

Select the stable receiver entry in HyperHDR and save it; choose BGR24, automatic
current resolution and automatic FPS when frame-size/interval enumeration is
absent. The mainline driver has G_PARM, but no generic S_PARM/frame-size API in
the inspected source. Do not force NV12: this tested source enumerates BGR3 only.
Start with tone mapping, crop, software decimation and smoothing disabled. Check
logs for driver/bus, capture API, returned dimensions, stride, planes and actual
quantization. The upstream source defaults BGR quantization to DEFAULT/sRGB;
only explicit LIM_RANGE enables RGB range expansion in this branch. The driver
also exposes read-only `V4L2_CID_DV_RX_RGB_RANGE` from the HDMI AVI infoframe;
record it using `v4l2-ctl --list-ctrls-menus` if gray ramps disagree with format
metadata. This branch does not override DEFAULT using that control. The board's
reported format and a gray-ramp test determine whether its source is correct.

## Application acceptance still required

Start HyperHDR before connecting HDMI, then connect it. Test live preview with
labeled corners/rows and independent RGB patches, gray ramps, black/white, and
moving content. A loopback capture can contain desktop feedback; a controlled
pattern is needed to judge orientation, channel order and freshness. Test crop
and quarter output independently after the full-size baseline.

Repeat HDMI disconnect/reconnect, source off/on, 1080p60 to 720p60 and back, and
capture enable/disable at least ten times. Verify return to fresh preview/LEDs
without restarting the service. Stop capture explicitly during retry and ensure
it stays stopped. Confirm retries are bounded, mappings/fds/RSS stabilize, and
no alternate camera is selected when the receiver disappears. After a genuine
node renumbering, confirm the saved identity resolves the new node. Avoid
renumbering a live capture node by removing drivers just for this test.

Test a conventional single-planar capture device separately. Then test WLED/DDP
and optical response using the existing hardware-validation and benchmark guides.
No such physical application tests were available in the development workspace.

## Copies and latency

Only the small v4l2_buffer identity is copied into a Linux worker. Mapped input
is retained until decoding completes; no full BGR frame is copied to a staging
buffer and no BGR-to-NV12 round trip is added. The decoder performs the necessary
BGR-to-RGB output write, combining optional range expansion/LUT in that pass.
Quarter mode samples half the width and height, reducing output pixels by four.

Measure full/quarter processing, worker-busy drops, capture/processing/output FPS
and optical latency before changing scheduling. Four capture buffers and workers
can hold frames under load; a future bounded latest-frame policy could improve
freshness but must preserve buffer ownership. Persistent DMA-BUF/RGA preprocessing
is a later optional experiment, not a requirement or a measured improvement here.

## Source evidence and limits

Inspected [Linux v6.18 snps_hdmirx.c](https://github.com/torvalds/linux/blob/v6.18/drivers/media/platform/synopsys/hdmirx/snps_hdmirx.c):
`hdmirx_v4l2_ioctl_ops`, `hdmirx_init_vb2_queue`, `hdmirx_query_dv_timings`,
`hdmirx_set_fmt`, `hdmirx_format_change`, and `hdmirx_querycap`.

That source registers standard vb2 MMAP buffers, one current format via ENUM_FMT,
TRY_FMT through G_FMT, source-change events and standard DV timings. It returns
ENOLINK/ENOLCK during acquisition, and requires buffers to be released before a
changed S_DV_TIMINGS. Per-row storage is aligned by the driver, demonstrating why
bytesperline and sizeimage must be preserved. This is an API/source audit of the
v6.18 base, not proof of every patch in the operator's 6.18.44 Armbian build.

The new automated tests cover stable identity derivation, DV timing metadata,
no-link/unlocked failures, timing change/failure/retry, exact 1080p BGR3 metadata,
padded stride, map-size rejection and cleanup. Existing buffer/decoder regression
suites remain applicable. Physical driver events, Qt retry scheduling and image
contents still require the application acceptance tests above.

## Verification results for this review

- Native x86_64 Linux application: PASS, full build with `ENABLE_V4L2=ON` and
  normal features retained, Debian Trixie prepared toolchain / GCC 14.2 / Qt 6.8.4.
- Capture suite: PASS, 13 groups, strict compiler warnings enabled.
- Real BGR24 decoder dispatch: PASS, 16 combinations of crop, scale, range and LUT.
- Address/undefined/leak sanitizer run: PASS for the capture suite inside Docker.
- Existing decoder comparisons: PASS, all 28 combinations, no errors/mismatches.
- `git diff --check`: PASS. No unresolved test or compiler failure.

These are development-host results. No new ARM64 package, physical HyperHDR
capture, mainline event/recovery loop, preview-content check, WLED test or
hardware latency measurement was performed. The earlier PR #1 was merged before
this review; these targeted corrections are a separate follow-up on the same
development branch. No merge or kernel action was performed during this review.

Implementation files: `include/grabber/linux/v4l2/V4L2Capture.h`,
`include/grabber/linux/v4l2/V4L2Grabber.h`,
`sources/grabber/linux/v4l2/V4L2Grabber.cpp`. Tests: `tests/v4l2/main.cpp` and its
README. Documentation: this review and the updated hardware-validation guide.
