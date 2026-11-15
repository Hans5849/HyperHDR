# Frame stages: captured, accepted, decoded, LED output

The RK3588 docs, logs and issues use five words for different things. A number for
one stage says nothing about the next, so results must always say which stage they
measure.

| Stage | Meaning | Where it is counted | Not to be confused with |
| --- | --- | --- | --- |
| **Captured** (dequeued) | The driver filled a buffer and `VIDIOC_DQBUF` returned it to HyperHDR. Includes buffers flagged `V4L2_BUF_FLAG_ERROR` and short payloads. | `dequeued` in the V4L2 capture statistics (#32). Gaps in `v4l2_buffer.sequence` count frames the driver dropped *before* this point. | The source's frame rate; `v4l2-ctl` loopback results (kernel-level only). |
| **Accepted** | A captured frame handed to a decode worker. Captured frames that aren't accepted are discarded for one recorded reason: driver error, short payload, software decimation (a setting, not a loss), undersized, workers inactive, LUT loading, or no idle worker. | `accepted` (#32) | Decoded: acceptance only means a worker started. |
| **Decoded** | The worker produced an RGB image (BGR24 → RGB, range expansion, optional LUT, crop or quarter). Otherwise the frame **failed to decode**, or was **abandoned** because capture restarted while it was in the worker. | `decoded`, `decodeFailed`, `abandoned` (#32, #34). Accept → decoded duration percentiles (#32). | The existing Performance page's "average frame time", which uses a millisecond clock and covers a different interval. |
| **Processed** | The decoded image reached HyperHDR's image processing (LED colour calculation). | Not counted separately. | — |
| **LED output** | An LED update sent by the LED driver (for WLED, one or more DDP packets). | Upstream's Performance page reports, per LED device, updates received and written and an estimate of missed refreshes. It doesn't link an LED update to the captured frame it came from, because smoothing can send LED updates on its own timer. | Captured or decoded frame rates. "N fps captured" is not "N LED updates per second from N frames". |

## What has and hasn't been measured

- **Decoder time.** Manual Cortex-A76 timings of the BGR24 decoder on a synthetic
  frame exist (see [development results](rk3588-development-results.md)). They are
  decoder-only, and not a reproducible baseline yet.
- **Accept → decoded time.** Instrumented by #32 once merged, but not yet measured
  on the board.
- **Not measured:**
  - HDMI-input-to-LED latency (end to end)
  - DDP send timing
  - WLED output rate or colour accuracy
  - any optical latency

  No document should claim these until a frame identifier is carried from capture
  to the LED driver and the result is measured on hardware. #32's instrumentation
  doc lists this as a follow-up.
- **Compilation and hosted CI** (x86_64 and ARM64 packages, sanitizer suites) show
  that the code builds and passes hardware-free tests. **They are not hardware
  validation.** The board status lives in
  [hardware validation](rk3588-hardware-validation.md).
