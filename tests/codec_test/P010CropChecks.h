#pragma once
// `DecoderTest --p010-crop`: P010 decoding through the real
// FrameDecoder::processImageVector (dispatchProcessImageVector, as GrabberWorker
// calls it) against P010's documented layout (issue #63):
//   Y plane: lineLength bytes per row, 16-bit little-endian samples, column c
//            at row * lineLength + c * 2;
//   UV plane after lineLength * height: one U,V pair of 16-bit samples per 2x2
//            pixels, the pair for column c at (row / 2) * lineLength + (c / 2) * 4.
// Without tone mapping a pixel is lut[LUT_INDEX(Y >> 8, U >> 8, V >> 8)]; with
// it, Y, U and V first go through FrameDecoderUtils' P010 tables (sample >> 6).
// Crops are pixels (left/right rounded down to even); quarter mode ignores them.
//
// Scope: crop arithmetic and values only. Buffers are sized as production
// allocates them (the image's 16 spare bytes, the LUT's 64-byte tail, slack
// after the source), because accesses past a row or frame are a separate
// defect (#61) with its own test. FrameDecoderUtils comes from main.cpp (the
// header has no include guard).
#include <utils/FrameDecoder.h>
#include <cstdio>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace P010Crop
{
struct Rgb { uint8_t r, g, b; };

// A distinct, non-separable colour per LUT index.
inline Rgb colour(uint32_t i)
{
	const uint32_t y = i & 0xFF, u = (i >> 8) & 0xFF, v = i >> 16;
	return { uint8_t(y * 7 + u * 3 + v), uint8_t(u ^ (y >> 1) ^ 0x5A), uint8_t(v + (y & 0x0F) * 11 + (u >> 4)) };
}

inline std::vector<uint8_t> buildLut()
{
	std::vector<uint8_t> lut(size_t(256) * 256 * 256 * 3 + 64);   // production tail (LUT_MEMORY_ALIGN)
	for (uint32_t i = 0; i < (1u << 24); ++i)
	{
		const Rgb c = colour(i);
		lut[size_t(i) * 3] = c.r; lut[size_t(i) * 3 + 1] = c.g; lut[size_t(i) * 3 + 2] = c.b;
	}
	return lut;
}

struct Case { int width, height, pad, cropLeft, cropRight, cropTop, cropBottom; bool quarter, tone; };

inline std::string label(const Case& c)
{
	char s[120];
	std::snprintf(s, sizeof(s), "P010 %dx%d line+%d crop L%d R%d T%d B%d%s%s", c.width, c.height, c.pad,
		c.cropLeft, c.cropRight, c.cropTop, c.cropBottom, c.quarter ? " quarter" : "", c.tone ? " tone-mapped" : "");
	return s;
}

inline std::vector<Case> cases()
{
	std::vector<Case> out;
	const int crops[][4] = { { 0, 0, 0, 0 }, { 2, 0, 0, 0 }, { 4, 2, 1, 1 }, { 6, 0, 2, 0 }, { 3, 1, 0, 0 }, { 0, 4, 0, 1 } };
	for (int w : { 8, 12, 18, 64 })
		for (int h : { 2, 4, 6 })
			for (int pad : { 0, 8 })
				for (bool quarter : { false, true })
					for (bool tone : { false, true })
						for (const auto& k : crops)
						{
							if (quarter && (k[0] || k[1] || k[2] || k[3]))
								continue;
							const int cl = (k[0] >> 1) << 1, cr = (k[1] >> 1) << 1;
							if (w - cl - cr <= 0 || h - k[2] - k[3] <= 0)
								continue;
							out.push_back({ w, h, pad, k[0], k[1], k[2], k[3], quarter, tone });
						}
	return out;
}

// Returns "" on a match, else the first difference.
inline std::string run(const Case& c, const std::vector<uint8_t>& lut, const std::array<uint8_t, 1024>& tabY, const std::array<uint8_t, 1024>& tabUV)
{
	const int line = c.width * 2 + c.pad;
	const size_t frame = size_t(line) * c.height + size_t(line) * (c.height / 2);
	std::vector<uint8_t> src(frame + 64);   // slack: reads past the frame are #61's subject
	uint32_t s = uint32_t(c.width * 131 + c.height * 17 + c.pad + c.cropLeft * 7) * 2654435761u + 1;
	for (auto& b : src) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; b = uint8_t(s >> 7); }

	const int cl = c.quarter ? 0 : (c.cropLeft >> 1) << 1, cr = c.quarter ? 0 : (c.cropRight >> 1) << 1;
	const int ct = c.quarter ? 0 : c.cropTop, cb = c.quarter ? 0 : c.cropBottom, step = c.quarter ? 2 : 1;
	const int outW = (c.width - cl - cr) / step, outH = (c.height - ct - cb) / step;

	Image<ColorRgb> image{ unsigned(outW), unsigned(outH) };
	FrameDecoder::dispatchProcessImageVector[c.quarter][c.tone][false](c.cropLeft, c.cropRight, c.cropTop, c.cropBottom,
		src.data(), nullptr, c.width, c.height, line, PixelFormat::P010, lut.data(), image, nullptr);
	if (int(image.width()) != outW || int(image.height()) != outH)
		return "output " + std::to_string(image.width()) + "x" + std::to_string(image.height()) + ", expected " + std::to_string(outW) + "x" + std::to_string(outH);

	auto sample = [&](size_t off) { return uint32_t(src[off]) | uint32_t(src[off + 1]) << 8; };
	for (int y = 0; y < outH; ++y)
		for (int x = 0; x < outW; ++x)
		{
			const int row = ct + y * step, col = cl + x * step;
			const uint32_t ys = sample(size_t(line) * row + size_t(col) * 2);
			const size_t uv = size_t(line) * c.height + size_t(line) * (row / 2) + size_t(col / 2) * 4;
			const uint32_t us = sample(uv), vs = sample(uv + 2);
			const uint32_t Y = c.tone ? tabY[ys >> 6] : ys >> 8, U = c.tone ? tabUV[us >> 6] : us >> 8, V = c.tone ? tabUV[vs >> 6] : vs >> 8;
			const Rgb want = colour(Y | U << 8 | V << 16);
			const auto& got = image(unsigned(x), unsigned(y));
			if (got.red != want.r || got.green != want.g || got.blue != want.b)
			{
				char m[160];
				std::snprintf(m, sizeof(m), "pixel (%d,%d) is %d,%d,%d, expected %d,%d,%d", x, y, got.red, got.green, got.blue, want.r, want.g, want.b);
				return m;
			}
		}
	return "";
}
}

inline int checkP010Crop()
{
	const auto lut = P010Crop::buildLut();
	const auto& ft = FrameDecoderUtils::instance();
	int failed = 0, passed = 0;
	for (const auto& c : P010Crop::cases())
	{
		const std::string err = P010Crop::run(c, lut, ft.getLutP010_y(), ft.getlutP010_uv());
		if (err.empty()) ++passed;
		else { ++failed; std::printf("FAIL %s: %s\n", P010Crop::label(c).c_str(), err.c_str()); }
	}
	if (failed) { std::printf("FAIL P010 crop: %d of %d cases\n", failed, failed + passed); return 1; }
	std::printf("PASS P010 crop: %d cases (crops, padding, quarter, tone mapping)\n", passed);
	return 0;
}
