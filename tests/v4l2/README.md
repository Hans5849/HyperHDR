# Hardware-free Linux V4L2 tests

```sh
cmake -S tests/v4l2 -B build/v4l2-tests
cmake --build build/v4l2-tests
ctest --test-dir build/v4l2-tests --output-on-failure
```

Only a C++17 compiler, CMake and Linux V4L2 headers are needed. The application
itself continues to use C++20. Fake syscalls test the production capability,
format and buffer helpers, including single-planar compatibility, one-plane
MPLANE, multi-plane ownership, partial mmap rollback, REQBUFS/QUERYBUF failure,
STREAMON/QBUF/DQBUF failure, EAGAIN, payload bounds, restart and cleanup. The
application accepts one contiguous memory plane; generic multi-plane allocation
is tested but is not a promise of multi-plane decoder support.

```sh
g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Iinclude tests/v4l2/main.cpp -o build/v4l2-tests/sanitized
build/v4l2-tests/sanitized
```

The actual FrameDecoder dispatch path is tested in `tests/codec_test`:

```sh
cmake -S tests/codec_test -B tests/codec_test/build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$PWD/tests/codec_test/build/bin"
cmake --build tests/codec_test/build --target DecoderTest --parallel 4
ctest --test-dir tests/codec_test/build --output-on-failure
```

Its BGR24 test covers 16 combinations of range, tone mapping, cropping and
full/quarter output using padded rows and asymmetric content. No generated image
or LUT file is needed for this CTest case. Do not build the default codec ALL
target just for this test: its existing fixture generator downloads an image.

To run the 28 existing decoder comparisons, generate fixtures from the local
pattern and provide a disposable home containing the LUT:

```sh
mkdir -p work/codec-home/.hyperhdr
codec_home="$PWD/work/codec-home"
tar -xJf resources/lut/lut_lin_tables.tar.xz -C "$codec_home/.hyperhdr"
python3 tests/codec_test/GenerateTestFrames.py --input resources/lut/rgb_test_pattern.png \
  --outdir tests/codec_test/build/bin
HOME="$codec_home" tests/codec_test/build/bin/DecoderTest > work/codec-regression.log 2>&1
# The legacy runner can return zero even when it reports failures. Inspect both.
if rg 'ERROR|mismatch' work/codec-regression.log; then exit 1; fi
```

The fixture generator requires Python numpy, pillow and requests. These tests do
not validate the Qt source-change/watchdog scheduling against an actual driver,
HDMI timing recovery, real DMA buffers or WLED output. Those remain hardware
acceptance tests documented in `docs/rk3588-hardware-validation.md`.

The mainline review also tests stable driver/bus identity, canonical DV timings,
no-link/unlocked acquisition failures, timing updates and set failures, the
verified 1080p60 BGR3 metadata, padded stride and QUERYBUF sizeimage checks.
See `docs/rk3588-mainline-review.md` for the physical evidence and remaining tests.
