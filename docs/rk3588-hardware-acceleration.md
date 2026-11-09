# RK3588 acceleration feasibility

## Recommendation

Prototype **optional RGA preprocessing after the native V4L2 path is verified on
hardware**. Keep the current CPU path as the default and fallback. No accelerator
is integrated in this branch. No acceleration speedup or latency measurement has
been made on an RK3588 in this environment.

At 1920x1080 BGR24, each frame is 6,220,800 bytes: 373,248,000 bytes/s at 60 fps
(about 356 MiB/s), before additional reads/writes. Reducing to 320x180 RGB takes
172,800 bytes/frame, 36 times fewer output bytes. This is a bandwidth calculation,
not a predicted 36x speedup. Capture DMA still writes the full input. The current
CPU quarter-frame option already skips pixels during conversion; RGA must beat
that inexpensive baseline, including submission, synchronization and cache costs.

## RGA

Rockchip's IM2D guide lists BGR_888/RGB_888, crop, resize and format conversion.
RK3588 has RGA2 and RGA3 engines; supported operations, scale ratios, addressability
and alignment depend on the selected engine and driver. RGA3's documented
BGR_888 width-stride alignment is 16 pixels; the verified 5760-byte line is 1920
BGR pixels and satisfies that alignment. Check all input/output/crop parameters
with `imcheck` and the actual board's library/driver rather than assuming every
advertised combination works. Very small outputs may need staged scaling because
of per-engine minimum sizes or scale limits. Pin an engine when comparing image
quality: Rockchip documents differing sample positions between RGA2 and RGA3.

Preferred future ownership flow:

1. Allocate the existing V4L2 MMAP ring. Try VIDIOC_EXPBUF for each memory plane
   once during stream setup. MMAP support does **not** prove EXPBUF support.
2. Import exported DMA-BUF fds once with `importbuffer_fd`, retain RGA handles for
   the ring lifetime, and allocate a small pool of DMA-backed output images.
   Wrap with the documented pixel strides, virtual dimensions and BGR_888 format.
3. DQBUF transfers a capture buffer to an in-flight job. Submit crop/resize and,
   if supported together, BGR-to-RGB conversion. Preserve source quantization;
   RGB limited-to-full expansion may need a small CPU pass on the reduced image.
   RGA color-conversion support does not establish correct RGB range expansion.
4. Wait for synchronous completion first; later use acquire/release fences with
   explicit fd ownership and deadlines. QBUF the input only when RGA has finished
   reading it. Wait for output completion before CPU LED analysis; apply required
   DMA-BUF CPU cache synchronization. V4L2 timestamps are not fence fds.
5. Run existing LUT, signal detection and LED analysis on the reduced image.
   Retain output storage until its consumer finishes. Bound in-flight jobs;
   favor fresh frames over an accumulating latency queue.
6. On timing change/failure, drain or cancel jobs, wait for fences, release RGA
   imports with `releasebuffer_handle`, close exported fds and output buffers,
   then unmap/release the V4L2 ring. Fall back to CPU without a permanent retry
   loop if driver versions, import, alignment or formats are unsupported.

If EXPBUF fails, first measure importing a persistent MMAP virtual address:
Rockchip documents virtual-address overhead and limitations. A capture mapping is
not automatically importable. Copying each full 6.2 MB frame into a DMA heap is a
last fallback for a prototype, and must be included in CPU/bandwidth/latency
measurements. Never label that path zero-copy. A reduced output copy may be cheap;
a full-frame copy may erase the gain. DMA heaps, IOMMU/address limits, cache
coherency and allocation permissions must be checked on the vendor kernel.

Use an optional Linux preprocessing interface at the capture-to-worker boundary,
with CPU and RGA implementations returning an owned image plus completion status.
Keep V4L2 plane descriptors and accelerator-specific handles out of shared image
and DDP APIs. A future `ENABLE_RGA` option should default OFF, require a compatible
`librga`/IM2D SDK and kernel RGA driver, and compile cleanly on other platforms.
Record SDK/driver versions and review distribution/licensing of the selected
SDK artifacts. Avoid runtime coupling of ordinary Linux captures to Rockchip.

## Mali-G610 GPU

A compute/fragment shader could combine BGR sampling, scaling and range conversion
and potentially LUT mapping. However, importing native BGR24 DMA-BUFs as EGL
images/Vulkan external memory is not guaranteed. BGR888 texture formats, DRM
modifiers, driver stack (vendor versus Mesa), synchronization and readback all
need testing. Upload plus synchronous CPU readback can outweigh the work saved
for a small LED image. A headless graphics context and optional dependencies add
substantial complexity. Evaluate only after the RGA prototype, or if an existing
GPU-resident processing chain eliminates transfers.

## NPU

RKNN inference is suited to a real learned operation such as scene classification
or semantic masking. Plain resizing, channel swapping and LED averaging do not
justify model conversion, quantization, tensor copies, inference scheduling and
RKNN runtime dependencies. No NPU work is recommended for baseline capture.

## Research gaps and acceptance experiment

Confirm EXPBUF on the receiver (`snps_hdmirx` on mainline); RGA imports of its allocations; available DMA heaps;
exact driver/SDK compatibility; BGR stride/size support; range expansion and LUT
order; fence/cache semantics; scale limits; output quality at borders; thermals;
CPU fallback and timeout cleanup. Compare CPU full/quarter output against RGA at
identical output sizes, plus 320x180/160x90 staged outputs where supported.
Adopt only if repeated runs reduce CPU time or optical latency without color,
recovery or dropped-frame regressions. See the benchmark plan.

## Sources consulted

Reviewed 2026-11-04; these links track moving branches, so pin revisions for a
prototype and record the actual installed board versions.

- [Rockchip librga](https://github.com/airockchip/librga), README and driver matrix.
- [IM2D developer guide](https://github.com/airockchip/librga/blob/main/docs/Rockchip_Developer_Guide_RGA_EN.md), format/stride tables, buffer imports and fences.
- [RGA FAQ](https://github.com/airockchip/librga/blob/main/docs/Rockchip_FAQ_RGA_EN.md), memory-import overhead, cache synchronization and RK3588 engine differences.
- [RKNN Toolkit2](https://github.com/airockchip/rknn-toolkit2), model conversion/inference role.
- [Linux V4L2 RGB formats](https://github.com/torvalds/linux/blob/master/Documentation/userspace-api/media/v4l/pixfmt-rgb.rst), component order and quantization context.
