#pragma once
// Reference cases for the other system/screen-grabber decoders (issue #51):
// FrameDecoder::processSystemImage{BGRA,RGBA,BGR16,PQ10}. These are reached
// through Grabber::processSystemFrame* by desktop, framebuffer and PipeWire
// grabbers. Not the V4L2 HDMI BGR24 decoder (Bgr24Checks.h, ColorQualityChecks.h).
//
// Qt-free: DecoderTest --system-decoders runs these against the real FrameDecoder.
// The geometry, LUTs and sampling rule are those of SystemBgrReference.h (#49):
//   output (x, y) = source pixel (startX + x*division, min(startY + y*division, height-1))
//   without LUT: the pixel's 8-bit R,G,B; with LUT: lut[LUT_INDEX(R,G,B) .. +2]
// Each case's row padding (lineSize - width*3 in SystemBgr::cases) is applied to
// the format's own pixel size, so odd strides exercise unaligned source rows.
// Buffers are allocated to their exact size: ASan reports reads past the last
// source pixel or the last LUT entry.
//
// Pixel layouts, as the grabbers deliver them (little-endian words):
//   BGRA   bytes B,G,R,A
//   RGBA   bytes R,G,B,A
//   BGR16  RGB565 word: R in bits 11-15, G in 5-10, B in 0-4; decoded as
//          R = r5<<3, G = g6<<2, B = b5<<3 (no low-bit replication)
//   PQ10   32-bit word: R10 in bits 0-9, G10 in 10-19, B10 in 20-29, 2 alpha bits;
//          decoded as the top 8 bits of each 10-bit value
#include "SystemBgrReference.h"

namespace SystemDecoder
{
using SystemBgr::Rgb;
using SystemBgr::Case;

enum class Format { BGRA, RGBA, BGR16, PQ10 };

inline const char* name(Format f)
{
	switch (f)
	{
		case Format::BGRA: return "BGRA";
		case Format::RGBA: return "RGBA";
		case Format::BGR16: return "BGR16";
		default: return "PQ10";
	}
}

inline int bytesPerPixel(Format f) { return f == Format::BGR16 ? 2 : 4; }

// Stride in bytes and the lineSize argument for the decoder (0: tight rows).
inline int stride(const Case& c, Format f)
{
	const int padding = c.lineSize ? c.lineSize - c.width * 3 : 0;
	return c.width * bytesPerPixel(f) + padding;
}
inline int lineSizeArgument(const Case& c, Format f) { return c.lineSize ? stride(c, f) : 0; }

// The 8-bit RGB a pixel of this colour decodes to in format f.
inline Rgb decoded(Format f, Rgb p)
{
	if (f == Format::BGR16)
		return { uint8_t(p.r & 0xF8), uint8_t(p.g & 0xFC), uint8_t(p.b & 0xF8) };
	return p;
}

inline void encode(Format f, Rgb p, int x, int y, uint8_t* s)
{
	switch (f)
	{
		case Format::BGRA: s[0] = p.b; s[1] = p.g; s[2] = p.r; s[3] = uint8_t(0x80 + x); break;
		case Format::RGBA: s[0] = p.r; s[1] = p.g; s[2] = p.b; s[3] = uint8_t(0x80 + y); break;
		case Format::BGR16:
		{
			const uint16_t v = uint16_t(((p.r >> 3) << 11) | ((p.g >> 2) << 5) | (p.b >> 3));
			s[0] = uint8_t(v); s[1] = uint8_t(v >> 8);
			break;
		}
		case Format::PQ10:
		{
			// Low 2 bits of each 10-bit value vary, so a decoder that used them
			// (or the wrong 8 of the 10) would be caught.
			const uint32_t r10 = (uint32_t(p.r) << 2) | uint32_t(x & 3);
			const uint32_t g10 = (uint32_t(p.g) << 2) | uint32_t(y & 3);
			const uint32_t b10 = (uint32_t(p.b) << 2) | uint32_t((x + y) & 3);
			const uint32_t v = r10 | (g10 << 10) | (b10 << 20) | (3u << 30);
			s[0] = uint8_t(v); s[1] = uint8_t(v >> 8); s[2] = uint8_t(v >> 16); s[3] = uint8_t(v >> 24);
			break;
		}
	}
}

// Exactly the bytes a grabber hands over: rows stride() apart, the last row
// without padding, padding bytes 0xEE.
inline std::vector<uint8_t> buildSource(const Case& c, Format f)
{
	const int bpp = bytesPerPixel(f), st = stride(c, f);
	std::vector<uint8_t> src(size_t(c.height - 1) * st + size_t(c.width) * bpp, 0xEE);
	for (int y = 0; y < c.height; ++y)
		for (int x = 0; x < c.width; ++x)
			encode(f, SystemBgr::colourAt(x, y), x, y, &src[size_t(y) * st + size_t(x) * bpp]);
	return src;
}

inline std::vector<uint8_t> expected(const Case& c, Format f, const std::function<Rgb(Rgb)>& map)
{
	std::vector<uint8_t> out;
	for (int y = 0; y < c.targetY; ++y)
		for (int x = 0; x < c.targetX; ++x)
		{
			const int sy = std::min(c.startY + y * c.division, c.height - 1);
			Rgb p = decoded(f, SystemBgr::colourAt(c.startX + x * c.division, sy));
			if (map) p = map(p);
			out.insert(out.end(), { p.r, p.g, p.b });
		}
	return out;
}

// decode(format, case, lineSize argument, source, lut or nullptr, output RGB)
using Decode = std::function<void(Format, const Case&, int, uint8_t*, uint8_t*, std::vector<uint8_t>&)>;

// Combinations not run, with the issue that tracks them (never silently).
using Skip = std::function<const char*(Format, bool withLut)>;

// Every format x case x LUT; one line each. Returns the number of failing combinations.
inline int run(const Decode& decode, const std::vector<Format>& formats, const Skip& skip = nullptr)
{
	int failed = 0;
	for (const auto& l : SystemBgr::luts())
	{
		std::vector<uint8_t> lut = l.map ? SystemBgr::buildLut(l.map) : std::vector<uint8_t>();
		for (Format f : formats)
			for (const auto& c : SystemBgr::cases())
			{
				if (const char* why = skip ? skip(f, bool(l.map)) : nullptr)
				{
					std::printf("SKIP processSystemImage%s, %s, %s: %s\n", name(f), c.name.c_str(), l.name.c_str(), why);
					continue;
				}
				std::vector<uint8_t> src = buildSource(c, f), got;
				decode(f, c, lineSizeArgument(c, f), src.data(), l.map ? lut.data() : nullptr, got);
				const auto want = expected(c, f, l.map);
				int wrong = 0, first = -1;
				for (size_t i = 0; i < want.size(); ++i)
					if (i >= got.size() || got[i] != want[i]) { if (first < 0) first = int(i); ++wrong; }
				failed += wrong != 0;
				if (wrong)
					std::printf("FAIL processSystemImage%s, %s, %s: %d of %zu channel values wrong (first: pixel %d channel %d got %d, expected %d)\n",
						name(f), c.name.c_str(), l.name.c_str(), wrong, want.size(), first / 3, first % 3,
						size_t(first) < got.size() ? got[size_t(first)] : -1, want[size_t(first)]);
				else
					std::printf("PASS processSystemImage%s, %s, %s\n", name(f), c.name.c_str(), l.name.c_str());
			}
	}
	return failed;
}
}
