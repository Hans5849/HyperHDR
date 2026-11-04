#pragma once
#include <utils/FrameDecoder.h>
#include <utils/Bgr24.h>
#include <iostream>
#include <vector>

// Exercise the real dispatch table, not a second implementation of the decoder.
inline int checkBgr24Decoder()
{
	constexpr int width = 6, height = 4, stride = 20;
	// No padding after the final pixel: catches accidental four-byte reads.
	std::vector<uint8_t> source((height - 1) * stride + width * 3, 0xee);
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			source[y * stride + x * 3] = 16 + x * 20;
			source[y * stride + x * 3 + 1] = 16 + y * 60;
			source[y * stride + x * 3 + 2] = x == 0 ? 235 : 200 - x * 15;
		}
	std::vector<uint8_t> lut(256 * 256 * 256 * 3);
	for (bool limited : {false, true})
		for (bool quarter : {false, true})
			for (bool tone : {false, true})
				for (bool crop : {false, true})
				{
					const int left = crop ? 2 : 0, top = crop ? 1 : 0, bottom = crop ? 1 : 0;
					const int outWidth = quarter ? width / 2 : width - left;
					const int outHeight = quarter ? height / 2 : height - top - bottom;
					for (int y = 0; y < height; ++y)
						for (int x = 0; x < width; ++x)
						{
							uint8_t rgb[3]; Bgr24::pixel<false>(&source[y * stride + x * 3], rgb, limited, nullptr);
							const auto index = (uint32_t(rgb[0]) | (uint32_t(rgb[1]) << 8) | (uint32_t(rgb[2]) << 16)) * 3;
							lut[index] = rgb[0] ^ 0x55; lut[index + 1] = rgb[1] ^ 0xaa; lut[index + 2] = rgb[2] ^ 0xff;
						}
					Image<ColorRgb> image;
					FrameDecoder::dispatchProcessImageVector[quarter][tone][false](left, 0, top, bottom,
						source.data(), nullptr, width, height, stride,
						limited ? PixelFormat::BGR24_LIMITED : PixelFormat::BGR24, lut.data(), image, nullptr);
					if (image.width() != unsigned(outWidth) || image.height() != unsigned(outHeight)) return 1;
					for (int y = 0; y < outHeight; ++y)
						for (int x = 0; x < outWidth; ++x)
						{
							const int sx = quarter ? x * 2 : x + left, sy = quarter ? y * 2 : y + top;
							const auto* p = &source[sy * stride + sx * 3];
							auto expected = [&](int c) { return limited ? Bgr24::expandLimited(p[c]) : p[c]; };
							const auto& actual = image(x, y);
							if (actual.red != (expected(2) ^ (tone ? 0x55 : 0)) ||
								actual.green != (expected(1) ^ (tone ? 0xaa : 0)) ||
								actual.blue != (expected(0) ^ (tone ? 0xff : 0)))
							{
								std::cerr << "FAIL BGR24: orientation, stride, crop, range or LUT mismatch\n";
								return 1;
							}
						}
				}
	std::cout << "PASS 16 BGR24 decoder combinations (range, scale, LUT, crop, padded stride, top-down rows)\n";
	return 0;
}
