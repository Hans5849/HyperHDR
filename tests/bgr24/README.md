# BGR24 reference tests

Hardware-free, Qt-free tests for `include/utils/Bgr24.h`, the conversion used for
the native RK3588 HDMI receiver (`BGR3`, top-down, 3 bytes per pixel). They run in
the hosted `CaptureTests` job on x86_64 and ARM64 with ASan/UBSan, and locally:

```sh
cmake -S tests/bgr24 -B build-bgr24 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"
cmake --build build-bgr24 && ctest --test-dir build-bgr24 --output-on-failure
```

## Why they exist

`tests/codec_test/Bgr24Checks.h` exercises the real `FrameDecoder` dispatch but
uses `Bgr24::expandLimited` as its expected value, the same function the lookup
table is built from. These tests recompute expectations independently:

- limited to full range in **exact integer rational** arithmetic and in
  **floating point**, checked against each other and against the code
- the LUT is built in its file order (blue outer, green, red inner, three bytes per
  entry) from a hash of (r, g, b, channel), then read through the decoder's index
  arithmetic, so a wrong index layout or an expansion applied after the LUT fails

## What is covered

| Area | Cases |
| --- | --- |
| Range expansion | all 256 inputs; clipping at 16 and 235; strictly increasing with steps of 1 or 2 inside the range; mid-gray 126 -> 128 |
| Rounding | output within 0.5 of the ideal real value; full -> limited -> full round trip within 1 code value |
| Channel order | source B, G, R becomes output R, G, B for black, white, primaries, secondaries and channel-id bytes |
| Gray ramps | full range is the identity; limited matches the reference; runtime flag equals the template choice |
| LUT | index layout, corners, axes, 20000 random pixels in both ranges; `(255,255,255)` reads the last 3 bytes of an exactly-sized buffer; identity LUT equals the direct path |
| Row layout | 22 widths including 1, odd, vector-unfriendly, 1919-1921 and 3839/3840; step 1 and 2; exact-size source (ASan catches any wider tail read); canaries around the output |
| Quarter output | step 2 takes even source pixels, no averaging |

## Expected numerical differences (documented, not defects)

- Limited to full range rounds to the nearest integer: worst error against the
  ideal real-valued mapping is 0.4932 (input 19). The 220 studio codes become 220
  distinct output codes, so the output skips some values (steps of 1 or 2).
- Full to limited and back differs by up to 1 code value; that is quantization,
  not a bug.
- With default or full quantization on a receiver that actually sends studio
  swing, black stays at 16 and white at 235 (no expansion). The grabber logs a
  mismatch warning for this instead of overriding the negotiated format.

## Not covered here

`FrameDecoder` dispatch (crop, stride, orientation, quarter size through the real
`dispatchProcessImageVector`) needs Qt and stays in `tests/codec_test`. The choice
of `BGR24` versus `BGR24_LIMITED` from the negotiated quantization is inline in
`V4L2Grabber.cpp` and isn't unit tested; the reporting helpers are covered in
`tests/v4l2`.

## Mutation check

Changing the rounding constant (to 100 or 120), the upper clamp (to 240), removing
rounding, swapping R and B, reversing the LUT index, applying the LUT before range
expansion, or reading one extra pixel at step 2 each makes the suite fail. Some
other edits (rounding constant 108 to 110, clamp at 236 instead of 235) don't
change any output for this ratio and are therefore not detectable.
