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
	// Test every image width, including 1..9 in border mode (#57).
	// With a zero-width border, middle rows must remain unchanged.
	const int failed = ApplyLut::run(apply);
	if (failed) { std::printf("FAIL applyLUT: %d case(s)\n", failed); return 1; }
	std::printf("PASS applyLUT: full-frame and border modes with no LUT, identity, inverting and channel-rotating LUTs\n");
	return 0;
}
