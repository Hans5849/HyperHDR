# Colour-quality references (issues #10, #12)

A backend-independent way to say whether a BGR24 capture path produces the right
colours in the right places. It complements, and doesn't repeat:

- `tests/bgr24`: `Bgr24.h` pixel and row functions against independent formulas
  (every range code, LUT index layout, row tails);
- `tests/codec_test --bgr24-check`: 16 decoder combinations on one 6x4 frame.

What this adds is a set of **reference cases run through the real
`FrameDecoder` dispatch**: odd sizes, odd and right/bottom crops, quarter
output, padded strides, the colour palette in both ranges, raw sub-black and
super-white codes, and known LUT transforms. It also adds a framework that a
future RGA or GPU backend can be measured against unchanged.

## Method

1. **Nominal colours in, codes out.** A case starts from what each pixel *means*
   (full-range RGB: black, white, mid-gray, primaries, secondaries, ramps). It is
   encoded as a receiver sends it: BGR byte order, top-down rows, padded stride,
   full or studio swing (`16 + round(v*219/255)`). The buffer is exactly
   `(h-1)*stride + w*3` bytes, so ASan catches any over-read, and padding is
   `0xA5`, so a stride error changes colours. `codeFrame` sends raw codes 0..255
   directly, including sub-black and super-white.
2. **Expected output from a written specification**, never from the code under
   test (`ColorReference.h` header comment): left/right crop rounded down to
   even, top/bottom not; quarter ignores crop, halves (rounding down) and samples
   pixel (2x, 2y); limited code c → `round((c-16)*255/219)` clamped, computed in
   exact integers and cross-checked against floating point for all 256 codes.
3. **Two comparisons per case, with explicit tolerances:**

   | Against | Full range | Limited range | Why |
   |---|---|---|---|
   | exact reference of the transmitted codes | 0 | 0 | the CPU path is integer-exact |
   | nominal source colour | 0 | **1** code value | studio swing has 220 levels; round trip error ≤ 1 (worst ideal error 0.4932) |

   LUT cases apply the transform (identity, invert, channel rotation) to the
   expected colour. These transforms move values by whole codes, so the
   tolerances are unchanged.
4. **Negative controls.** Eight plausible wrong decoders must each fail at least
   one case: R/B swapped, bottom-up rows, stride ignored, quarter averaging pairs,
   odd left crop not aligned, LUT indexed `b|g<<8|r<<16`, range applied after the
   LUT, limited treated as full. This shows the references discriminate.

## Running

```sh
cmake -S tests/color_reference -B build-color-reference -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"
cmake --build build-color-reference && ctest --test-dir build-color-reference --output-on-failure
# Real dispatch (needs Qt 6 and libturbojpeg):
build-codec/bin/DecoderTest --color-quality      # also run by ctest in build-codec
```

Hosted CI runs both on x86_64 and ARM64. The board workflow's ASan decoder step
runs the real-dispatch check through `ctest` as well.

## Regression results (2026-11-15)

32 cases (16 per range), CPU row backend locally under ASan/UBSan, and the
dispatch harness locally (a stub decoder whose BGR24 branch is copied from
`FrameDecoder.cpp`, because the real one needs Qt). The real `FrameDecoder` run
is in hosted CI.

| Group | Cases | Max error vs codes | Max error vs nominal |
|---|---:|---|---|
| full range (palette, ramps, raw codes, crops, quarter, LUTs) | 16 | 0/0/0 | 0/0/0 |
| limited, palette (0, 128, 255 levels), no LUT | 8 | 0/0/0 | 0/0/0 |
| limited, ramps (all nominal values), no LUT | 2 | 0/0/0 | ≤ 1 (1/0/1 and 1/1/1) |
| limited, raw codes 0..255 (one with LUT invert) | 2 | 0/0/0 | 0/0/0 (nominal = clipped decode) |
| limited, LUT invert / rotate on ramps | 2 | 0/0/0 | 1/1/1 |
| limited, LUT identity / rotate on palette | 2 | 0/0/0 | 0/0/0 |

Sensitivity to the production code: temporarily changing `Bgr24.h` (rounding
constant 109 → 120; lower clamp 16 → 20; upper clamp 235 → 240; R/B swapped;
LUT output bytes rotated; quarter step ignored) makes the suite fail in every
case. The upper-clamp change survived until raw-code frames were added: encoded
frames never contain codes above 235.

## Using it for RGA or GPU output

Write a `ColorReference::Backend` that converts `Case::frame` with the backend and
returns packed RGB, then call `ColorReference::run(standardCases(), backend)`.
Expected values and comparisons stay the same; only the backend's declared
tolerances may change, and they must be stated per case rather than widened
globally. Two differences to expect and handle explicitly:

- **Range conversion rounding**: a hardware CSC may round differently. If it
  differs by at most 1 code from the exact reference, raise `toleranceExact` to 1
  for that backend and record it.
- **Scaling**: RGA downscales with a filter, not by sampling (2x, 2y). Quarter
  cases need a "filtered" expectation (2x2 box average) or must be compared on
  flat regions only. That's a new expectation mode, not a looser tolerance.
