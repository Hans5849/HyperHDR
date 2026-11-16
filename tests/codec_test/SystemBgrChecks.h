#pragma once
// `DecoderTest --system-bgr`: the real FrameDecoder::processSystemImageBGR
// against the independent reference in SystemBgrReference.h (issue #49).
#include <utils/FrameDecoder.h>
#include "SystemBgrReference.h"
#include <cstdio>

inline int checkSystemBgr()
{
	const SystemBgr::Decode decode = [](const SystemBgr::Case& c, uint8_t* src, uint8_t* lut, std::vector<uint8_t>& rgb) {
		Image<ColorRgb> image(c.targetX, c.targetY);
		FrameDecoder::processSystemImageBGR(image, c.targetX, c.targetY, c.startX, c.startY, src, c.width, c.height, c.division, lut, c.lineSize);
		rgb.resize(size_t(c.targetX) * c.targetY * 3);
		for (int y = 0; y < c.targetY; ++y)
			for (int x = 0; x < c.targetX; ++x)
			{
				const auto& p = image(x, y);
				uint8_t* o = &rgb[(size_t(y) * c.targetX + x) * 3];
				o[0] = p.red; o[1] = p.green; o[2] = p.blue;
			}
	};
	const int failed = SystemBgr::run(decode, "processSystemImageBGR");
	if (failed) { std::printf("FAIL system BGR: %d case(s)\n", failed); return 1; }
	std::printf("PASS system BGR: every case with no LUT, identity, inverting and channel-rotating LUTs\n");
	return 0;
}
