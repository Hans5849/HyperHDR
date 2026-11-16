#pragma once
// `DecoderTest --system-decoders`: the real FrameDecoder::processSystemImage
// {BGRA,RGBA,BGR16,PQ10} against the independent reference in
// SystemDecoderReference.h (issue #51), under the CI's ASan/UBSan.
#include <utils/FrameDecoder.h>
#include "SystemDecoderReference.h"
#include <cstdio>

inline int checkSystemDecoders()
{
	using namespace SystemDecoder;
	const Decode decode = [](Format f, const Case& c, int lineSize, uint8_t* src, uint8_t* lut, std::vector<uint8_t>& rgb) {
		Image<ColorRgb> image(c.targetX, c.targetY);
		switch (f)
		{
			case Format::BGRA: FrameDecoder::processSystemImageBGRA(image, c.targetX, c.targetY, c.startX, c.startY, src, c.width, c.height, c.division, lut, lineSize); break;
			case Format::RGBA: FrameDecoder::processSystemImageRGBA(image, c.targetX, c.targetY, c.startX, c.startY, src, c.width, c.height, c.division, lut, lineSize); break;
			case Format::BGR16: FrameDecoder::processSystemImageBGR16(image, c.targetX, c.targetY, c.startX, c.startY, src, c.width, c.height, c.division, lut, lineSize); break;
			case Format::PQ10: FrameDecoder::processSystemImagePQ10(image, c.targetX, c.targetY, c.startX, c.startY, src, c.width, c.height, c.division, lut, lineSize); break;
		}
		rgb.resize(size_t(c.targetX) * c.targetY * 3);
		for (int y = 0; y < c.targetY; ++y)
			for (int x = 0; x < c.targetX; ++x)
			{
				const auto& p = image(x, y);
				uint8_t* o = &rgb[(size_t(y) * c.targetX + x) * 3];
				o[0] = p.red; o[1] = p.green; o[2] = p.blue;
			}
	};
	const int failed = run(decode, { Format::BGRA, Format::RGBA, Format::BGR16, Format::PQ10 });
	if (failed) { std::printf("FAIL system decoders: %d combination(s)\n", failed); return 1; }
	std::printf("PASS system decoders: BGRA, RGBA, BGR16, PQ10, every case with no LUT, identity, inverting and channel-rotating LUTs\n");
	return 0;
}
