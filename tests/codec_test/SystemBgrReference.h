#pragma once
// Reference cases for FrameDecoder::processSystemImageBGR (issue #49): the
// system/screen-grabber path for packed BGR frames (Linux framebuffer and
// Amlogic grabbers, through Grabber::processSystemFrameBGR). Not the V4L2 HDMI
// BGR24 decoder, which is covered by Bgr24Checks.h and ColorQualityChecks.h.
//
// Qt-free: DecoderTest --system-bgr runs these against the real FrameDecoder.
//
// The expected image is computed here from the pixel colours and the documented
// sampling rule, independently of the decoder:
//   output (x, y) = source pixel (startX + x*division, min(startY + y*division, height-1))
//   source rows are lineSize bytes apart (width*3 when lineSize is 0), pixels are B,G,R
//   without LUT: R,G,B; with LUT: lut[LUT_INDEX(R,G,B) .. +2]
// Buffers are allocated to their exact size, so ASan reports any read past the
// last source pixel or the last LUT entry (the LUTs here have no padding).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace SystemBgr
{
struct Rgb { uint8_t r, g, b; };

struct Case
{
	std::string name;
	int width, height, lineSize;              // lineSize 0: tightly packed
	int startX, startY, division, targetX, targetY;
};

// One full-size RGB LUT, laid out as LutLoader loads it (index LUT_INDEX(r,g,b)
// = (r + g*256 + b*65536) * 3, entries R,G,B), without LutLoader's 64-byte tail.
struct Lut
{
	std::string name;
	std::function<Rgb(Rgb)> map;   // nullptr: no LUT
};

inline Rgb colourAt(int x, int y)
{
	// Distinct, asymmetric colours per pixel; includes 0 and 255 on every
	// channel (the last LUT entry is white) and R != B everywhere else.
	static const Rgb fixed[] = { {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}, {0, 0, 0}, {10, 20, 30}, {200, 100, 50}, {1, 2, 3} };
	const int i = y * 17 + x;
	if (i < 8) return fixed[i];
	return { uint8_t(37 * x + 11 * y + 5), uint8_t(91 * y + 7 * x + 60), uint8_t(13 * x * y + 150) };
}

inline std::vector<Case> cases()
{
	return {
		{ "4x2, padded stride, full size", 4, 2, 16, 0, 0, 1, 4, 2 },
		{ "4x2, tight rows (lineSize 0 -> width*3)", 4, 2, 0, 0, 0, 1, 4, 2 },
		{ "7x5, crop 1,1, division 2, padded stride", 7, 5, 24, 1, 1, 2, 3, 2 },
		{ "6x5, crop 1,3, division 2, last row clamped", 6, 5, 20, 1, 3, 2, 2, 2 },
		{ "9x3, last column and last row reached", 9, 3, 27, 2, 0, 3, 3, 3 },
	};
}

inline std::vector<Lut> luts()
{
	return {
		{ "no LUT", nullptr },
		{ "identity LUT", [](Rgb c) { return c; } },
		{ "inverting LUT", [](Rgb c) { return Rgb{ uint8_t(255 - c.r), uint8_t(255 - c.g), uint8_t(255 - c.b) }; } },
		{ "channel-rotating LUT (R<-G, G<-B, B<-R)", [](Rgb c) { return Rgb{ c.g, c.b, c.r }; } },
	};
}

inline std::vector<uint8_t> buildLut(const std::function<Rgb(Rgb)>& map)
{
	std::vector<uint8_t> lut(size_t(256) * 256 * 256 * 3);
	for (int b = 0; b < 256; ++b)
		for (int g = 0; g < 256; ++g)
			for (int r = 0; r < 256; ++r)
			{
				const Rgb o = map(Rgb{ uint8_t(r), uint8_t(g), uint8_t(b) });
				uint8_t* e = &lut[(size_t(r) + size_t(g) * 256 + size_t(b) * 65536) * 3];
				e[0] = o.r; e[1] = o.g; e[2] = o.b;
			}
	return lut;
}

// Exactly the bytes a grabber hands over: rows lineSize apart, the last row
// without padding, padding bytes 0xEE.
inline std::vector<uint8_t> buildSource(const Case& c)
{
	const int stride = c.lineSize ? c.lineSize : c.width * 3;
	std::vector<uint8_t> src(size_t(c.height - 1) * stride + size_t(c.width) * 3, 0xEE);
	for (int y = 0; y < c.height; ++y)
		for (int x = 0; x < c.width; ++x)
		{
			const Rgb p = colourAt(x, y);
			uint8_t* s = &src[size_t(y) * stride + size_t(x) * 3];
			s[0] = p.b; s[1] = p.g; s[2] = p.r;
		}
	return src;
}

inline std::vector<uint8_t> expected(const Case& c, const std::function<Rgb(Rgb)>& map)
{
	std::vector<uint8_t> out;
	for (int y = 0; y < c.targetY; ++y)
		for (int x = 0; x < c.targetX; ++x)
		{
			const int sy = std::min(c.startY + y * c.division, c.height - 1);
			Rgb p = colourAt(c.startX + x * c.division, sy);
			if (map) p = map(p);
			out.insert(out.end(), { p.r, p.g, p.b });
		}
	return out;
}

// decode(case, source, lut or nullptr, output RGB of targetX*targetY*3 bytes)
using Decode = std::function<void(const Case&, uint8_t*, uint8_t*, std::vector<uint8_t>&)>;

// Runs every case with every LUT; prints one line each. Returns failures.
inline int run(const Decode& decode, const char* label)
{
	int failed = 0;
	for (const auto& l : luts())
	{
		std::vector<uint8_t> lut = l.map ? buildLut(l.map) : std::vector<uint8_t>();
		for (const auto& c : cases())
		{
			std::vector<uint8_t> src = buildSource(c), got;
			decode(c, src.data(), l.map ? lut.data() : nullptr, got);
			const auto want = expected(c, l.map);
			int wrong = 0, first = -1;
			for (size_t i = 0; i < want.size(); ++i)
				if (i >= got.size() || got[i] != want[i]) { if (first < 0) first = int(i); ++wrong; }
			failed += wrong != 0;
			if (wrong)
				std::printf("FAIL %s, %s, %s: %d of %zu channel values wrong (first: pixel %d channel %d got %d, expected %d)\n",
					label, c.name.c_str(), l.name.c_str(), wrong, want.size(), first / 3, first % 3,
					size_t(first) < got.size() ? got[size_t(first)] : -1, want[size_t(first)]);
			else
				std::printf("PASS %s, %s, %s\n", label, c.name.c_str(), l.name.c_str());
		}
	}
	return failed;
}
}
