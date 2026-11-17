#pragma once
// Reference cases for FrameDecoder::applyLUT (issue #56): applies a full RGB
// LUT in place to a packed RGB image. Used by FlatBuffersServer for RGB
// FlatBuffers and Protobuf images when tone mapping is on. Not used by the V4L2
// HDMI path or the system grabbers.
//
// Qt-free: DecoderTest --apply-lut runs these against the real FrameDecoder.
// The expected image is computed here, independently of the decoder:
//   mode 1 (full frame): every pixel p becomes lut[LUT_INDEX(p.r, p.g, p.b)]
//   mode 2 (border only), with sizeX = width*10/100 and sizeY = height*25/100:
//     rows y < sizeY or y > height - sizeY: every pixel
//     other rows: pixels x < sizeX and x >= width - sizeX; the rest unchanged
//   (the bottom border is one row shorter than the top one: that is the
//   function's existing behaviour, kept as is)
//   any mode with no LUT (nullptr) or mode 0: the image is unchanged
// The image buffer is exactly width*height*3 bytes and the LUT has no padding,
// so ASan sees reads or writes outside them. The first case reads its second
// pixel at byte offset 3, well inside the buffer, so UBSan reports a
// misaligned 4-byte load there before any case reaches the buffer end.
#include "SystemBgrReference.h"

namespace ApplyLut
{
using SystemBgr::Rgb;

struct Case { int width, height; const char* note; };

inline std::vector<Case> cases()
{
	return {
		{ 4, 2, "pixel 1 at offset 3, inside the buffer" },
		{ 1, 1, "single pixel" },
		{ 2, 1, "two pixels, second at offset 3" },
		{ 3, 2, "odd width, row boundary at byte 9" },
		{ 7, 5, "odd size" },
		{ 10, 4, "border: sizeX 1, sizeY 1" },
		{ 13, 9, "odd border: sizeX 1, sizeY 2" },
		{ 40, 8, "border: sizeX 4, sizeY 2" },
		{ 33, 17, "odd border: sizeX 3, sizeY 4" },
	};
}

// Distinct colours per pixel, including 0 and 255 on every channel (white is
// the last LUT entry) and R != B so channel-order mistakes show.
inline Rgb colourAt(int x, int y)
{
	return SystemBgr::colourAt(x, y);
}

inline std::vector<uint8_t> image(const Case& c)
{
	std::vector<uint8_t> img(size_t(c.width) * c.height * 3);
	for (int y = 0; y < c.height; ++y)
		for (int x = 0; x < c.width; ++x)
		{
			const Rgb p = colourAt(x, y);
			uint8_t* d = &img[(size_t(y) * c.width + x) * 3];
			d[0] = p.r; d[1] = p.g; d[2] = p.b;
		}
	return img;
}

inline bool processed(const Case& c, int mode, int x, int y)
{
	if (mode == 1)
		return true;
	const int sizeX = (c.width * 10) / 100, sizeY = (c.height * 25) / 100;
	if (y < sizeY || y > c.height - sizeY)
		return true;
	return x < sizeX || x >= c.width - sizeX;
}

inline std::vector<uint8_t> expected(const Case& c, int mode, const std::function<Rgb(Rgb)>& map)
{
	std::vector<uint8_t> out = image(c);
	if (!map || mode == 0)
		return out;
	for (int y = 0; y < c.height; ++y)
		for (int x = 0; x < c.width; ++x)
			if (processed(c, mode, x, y))
			{
				const Rgb p = map(colourAt(x, y));
				uint8_t* d = &out[(size_t(y) * c.width + x) * 3];
				d[0] = p.r; d[1] = p.g; d[2] = p.b;
			}
	return out;
}

// apply(case, image (in place), lut or nullptr, mode)
using Apply = std::function<void(const Case&, std::vector<uint8_t>&, const uint8_t*, int)>;
using Skip = std::function<const char*(const Case&, int mode, bool lut)>;

// Every case x mode {1, 2} x LUT {none, identity, inverting, channel-rotating},
// plus mode 0 with a LUT. One line each. Returns the number of failures.
inline int run(const Apply& apply, const Skip& skip = nullptr)
{
	int failed = 0;
	for (const auto& l : SystemBgr::luts())
	{
		std::vector<uint8_t> lut = l.map ? SystemBgr::buildLut(l.map) : std::vector<uint8_t>();
		for (int mode : { 0, 1, 2 })
		{
			if (mode == 0 && !l.map)
				continue;
			for (const auto& c : cases())
			{
				char label[160];
				std::snprintf(label, sizeof(label), "applyLUT %dx%d (%s), mode %d, %s", c.width, c.height, c.note, mode, l.name.c_str());
				if (const char* why = skip ? skip(c, mode, bool(l.map)) : nullptr)
				{
					std::printf("SKIP %s: %s\n", label, why);
					continue;
				}
				std::vector<uint8_t> img = image(c);
				apply(c, img, l.map ? lut.data() : nullptr, mode);
				const auto want = expected(c, mode, l.map);
				int wrong = 0, first = -1;
				for (size_t i = 0; i < want.size(); ++i)
					if (img[i] != want[i]) { if (first < 0) first = int(i); ++wrong; }
				failed += wrong != 0;
				if (wrong)
					std::printf("FAIL %s: %d of %zu values wrong (first: pixel %d channel %d got %d, expected %d)\n",
						label, wrong, want.size(), first / 3, first % 3, img[size_t(first)], want[size_t(first)]);
				else
					std::printf("PASS %s\n", label);
			}
		}
	}
	return failed;
}
}
