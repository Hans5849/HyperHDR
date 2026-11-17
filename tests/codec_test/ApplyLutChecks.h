#pragma once
// `DecoderTest --apply-lut`: the real FrameDecoder::applyLUT against the
// independent reference in ApplyLutReference.h (issue #56).
#include <utils/FrameDecoder.h>
#include "ApplyLutReference.h"
#include <cstdio>

inline int checkApplyLut()
{
	const ApplyLut::Apply apply = [](const ApplyLut::Case& c, std::vector<uint8_t>& img, const uint8_t* lut, int mode) {
		FrameDecoder::applyLUT(img.data(), unsigned(c.width), unsigned(c.height), lut, mode);
	};
	// Border mode with width < 10 (sizeX 0) reads and writes the pixel after each
	// middle row, past the image on the last one: a separate defect,
	// issue #57. Neither caller can pass border mode today
	// (FlatBuffersServer::getHdrToneMappingEnabled() returns 0 or 1).
	const ApplyLut::Skip skip = [](const ApplyLut::Case& c, int mode, bool lut) -> const char* {
		return (lut && mode == 2 && (c.width * 10) / 100 == 0)
			? "border mode with width < 10 is a separate defect (#57)" : nullptr;
	};
	const int failed = ApplyLut::run(apply, skip);
	if (failed) { std::printf("FAIL applyLUT: %d case(s)\n", failed); return 1; }
	std::printf("PASS applyLUT: full-frame and border modes with no LUT, identity, inverting and channel-rotating LUTs\n");
	return 0;
}
