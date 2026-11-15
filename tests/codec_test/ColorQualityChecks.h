#pragma once
// The colour-quality reference cases (tests/color_reference/ColorReference.h)
// through the real FrameDecoder BGR24 dispatch: odd sizes and crops, right and
// bottom crop, quarter output, padded strides, the reference palette in both
// ranges and known LUT transforms. `DecoderTest --color-quality`.
#include <utils/FrameDecoder.h>
#include "../color_reference/ColorReference.h"
#include <cstdio>

inline int checkColorQuality()
{
	const ColorReference::Backend dispatch = [](const ColorReference::Case& c, const uint8_t* lut, std::vector<uint8_t>& rgb, int& w, int& h) {
		const auto& f = c.frame;
		const auto& g = c.geometry;
		Image<ColorRgb> image;
		FrameDecoder::dispatchProcessImageVector[g.quarter][lut != nullptr][false](g.cropLeft, g.cropRight, g.cropTop, g.cropBottom,
			f.bytes.data(), nullptr, f.width, f.height, f.stride,
			f.limited ? PixelFormat::BGR24_LIMITED : PixelFormat::BGR24, lut, image, nullptr);
		w = int(image.width());
		h = int(image.height());
		rgb.resize(size_t(w) * h * 3);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const auto& p = image(x, y);
				uint8_t* o = &rgb[(size_t(y) * w + x) * 3];
				o[0] = p.red; o[1] = p.green; o[2] = p.blue;
			}
		return true;
	};
	int failed = 0;
	for (const auto& r : ColorReference::run(ColorReference::standardCases(), dispatch))
	{
		const bool pass = r.ran && r.exact.ok() && r.nominal.ok();
		failed += !pass;
		std::printf("%s %s: exact %s; nominal %s\n", pass ? "PASS" : "FAIL", r.name.c_str(), r.exact.describe().c_str(), r.nominal.describe().c_str());
	}
	if (failed) { std::printf("FAIL colour quality: %d case(s)\n", failed); return 1; }
	std::printf("PASS colour quality: all reference cases through FrameDecoder (exact vs codes; nominal within 1 for limited range)\n");
	return 0;
}
