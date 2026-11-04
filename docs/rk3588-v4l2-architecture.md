# V4L2 architecture audit

Audited `b1451f2868072c5b8eda0ae61c0fcf33ecc1bfc5` before changing capture code.

## Existing path

`V4L2Grabber::enumerateV4L2devices` walks `/sys/class/video4linux`, opens
nodes nonblocking, queries capabilities, controls, inputs, formats, discrete
frame sizes and intervals. It builds `DeviceProperties` for normal UI selection.
A fallback to the current G_FMT exists, but was hardcoded to `/dev/video0` and
could use an uninitialized input index. Devices without interval enumeration
are relevant to native HDMI receivers.

Every G_FMT, ENUM_FMT, S_FMT, crop, stream-parameter and buffer ioctl uses
VIDEO_CAPTURE. No VIDEO_CAPTURE_MPLANE or v4l2_plane support exists. Runtime
validation uses global capabilities, ignoring DEVICE_CAPS. The FFmpeg symptom
therefore agrees with source evidence, but was not used as proof.

`init_device` uses G_FMT then S_FMT, followed by frame interval and controls.
It reads pix.bytesperline but uses requested rather than returned dimensions.
Four MMAP buffers are requested; QUERYBUF returns a single offset/length.
QSocketNotifier drives nonblocking DQBUF. A worker receives a shallow copy of
v4l2_buffer plus a direct pointer into the mapping. QBUF happens on worker
completion, retaining the mapping throughout processing without an input copy.
Plane arrays added on the read_frame stack must never be used through that
shallow copy after read_frame returns.

`FrameDecoder` handles conversion, crop, optional half-width/half-height output
and LUT tone mapping. Its legacy RGB24 path actually reads B,G,R bytes and
vertically reverses rows (also used by other capture backends). Consequently,
reusing its name for native top-down BGR3 would produce an upside-down image.
There is no explicit limited-range RGB decoder. YUV conversion depends on a LUT.
Existing contiguous I420/NV12/P010 formats must not be confused with separate
memory-plane variants such as NV12M.

The helpers named throw_exception/throw_errno_exception only log. Some failure
paths return with fd/mappings alive; uninit is conditional on successful startup.
DQBUF EIO stops capture, but QBUF failures do not reliably stop it. There is no
capture watchdog, source-change subscription, or local retry backoff.

## Minimal implementation boundary

Keep discovery, worker dispatch and DDP output intact. Add a small Linux-only,
Qt-independent V4L2 helper for capability selection, normalized formats and
owned MMAP planes, injectable at the syscall boundary for tests. Rebuild QBUF
metadata from a worker's buffer index; do not retain transient plane pointers.
Prefer VIDEO_CAPTURE when a node offers both APIs to preserve existing behavior.
Use pix_mp only for MPLANE, and initially accept exactly one memory plane in the
application decoder. Reject separate-plane layouts explicitly before mapping;
the generic mapping owner can still be tested for multiple-plane cleanup.

Add a distinct top-down BGR24 decoder, with limited-range expansion before the
RGB LUT and no full-frame staging copy. Preserve legacy decoder behavior.
Use returned negotiated dimensions/stride, check payload bounds, clean partial
startup, and use bounded timer retries for stream failures and source changes.
No board names, fixed resolution, acceleration or new YUV dependencies belong
in the common path. Hardware verification remains a separate requirement.

## Implemented result

The helper now normalizes pix/pix_mp, selects the node's effective capabilities,
probes MPLANE modes with TRY_FMT (rejecting substituted formats and rejected NV12),
and owns MMAP planes. Receivers without discrete frame-size/input enumeration
can advertise usable formats at their current timing; BGR24 is preferred in that
fallback. Current-format fallback is no longer tied to a numbered node.

Workers retain a mapping until completion, but return buffers using fresh QBUF
metadata reconstructed from their index. Startup failure and normal stop both
join workers, STREAMOFF, unmap all planes, release REQBUFS and close the fd.
Frame generations discard queued completions from a prior stream. Source-change
notifications, fatal buffer errors and a five-second watchdog cause teardown and
three-second retry. Explicit stop cancels retries; EAGAIN readiness is throttled.

BGR3 has its own top-down, stride-aware decoder and limited-range variant.
Range expansion and optional RGB LUT lookup share the output conversion pass.
Legacy RGB24/XRGB semantics remain unchanged. Separate-memory-plane decoder
layouts are intentionally unsupported; the mapping owner's generic multi-plane
rollback is nevertheless covered by tests. Kernel event/timing behavior and
physical LED output still require the hardware acceptance procedure.
