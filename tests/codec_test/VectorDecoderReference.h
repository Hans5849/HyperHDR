#pragma once
// Reference cases for FrameDecoder::processImageVector and the VECTOR_* row
// decoders in VectorizedDecoders.h (issue #61): the YUYV, UYVY, NV12, I420,
// P010, RGB24 and XRGB paths used by USB and other grabbers on every platform,
// and by the FlatBuffers server. (BGR24 has its own decoder and tests.)
//
// Qt-free. The expected image is computed here from each format's documented
// memory layout, not from the decoder's pointer arithmetic:
//   lineLength (L) is the stride of the first plane in bytes; crops are pixels,
//   left and right rounded down to even; quarter mode drops all crops and takes
//   every second column and row. YUV formats: output (x, y) samples source
//   column cropLeft + x*step, row cropTop + y*step.
//   YUYV  row: Y0 U Y1 V per 2 pixels.          UYVY: U Y0 V Y1.
//   NV12  Y plane L*H, then interleaved U V, stride L, one pair per 2x2 pixels.
//   I420  Y plane L*H, then U and V planes, stride L/2, one sample per 2x2.
//   P010  as NV12 with 16-bit little-endian samples (stride L bytes); the
//         decoder uses the high byte, or with tone mapping the 10-bit value
//         (sample >> 6) through FrameDecoderUtils' P010 tables.
//   RGB24 / XRGB: bottom-up rows (output row 0 is source row H - cropBottom -
//         1), pixels B G R (X); without tone mapping output is R G B, with it
//         lut[LUT_INDEX(R, G, B)].
//   A YUV pixel is lut[LUT_INDEX(Y, U, V)] = lut[(Y | U << 8 | V << 16) * 3].
// Buffers are exact: the source is lineLength*height (plus chroma planes), the
// LUT 256^3*3 with no tail.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <functional>
#include <string>
#include <vector>

namespace VectorRef
{
enum class Fmt { YUYV, UYVY, NV12, I420, P010, RGB24, XRGB };

inline const char* name(Fmt f)
{
	switch (f)
	{
		case Fmt::YUYV: return "YUYV";
		case Fmt::UYVY: return "UYVY";
		case Fmt::NV12: return "NV12";
		case Fmt::I420: return "I420";
		case Fmt::P010: return "P010";
		case Fmt::RGB24: return "RGB24";
		default: return "XRGB";
	}
}

inline bool isRgb(Fmt f) { return f == Fmt::RGB24 || f == Fmt::XRGB; }
inline bool isPlanar(Fmt f) { return f == Fmt::NV12 || f == Fmt::I420 || f == Fmt::P010; }

inline int bytesPerPixel(Fmt f)
{
	switch (f)
	{
		case Fmt::NV12: case Fmt::I420: return 1;
		case Fmt::RGB24: return 3;
		case Fmt::XRGB: return 4;
		default: return 2;
	}
}

// Distinct, non-separable colour for every LUT index: swapping Y/U/V or R/G/B,
// or an off-by-one index, gives a different colour.
struct Rgb { uint8_t r, g, b; };
inline Rgb lutColour(uint32_t index)
{
	const uint32_t y = index & 0xFF, u = (index >> 8) & 0xFF, v = index >> 16;
	return { uint8_t(y * 7 + u * 3 + v), uint8_t(u ^ (y >> 1) ^ 0x5A), uint8_t(v + (y & 0x0F) * 11 + (u >> 4)) };
}

inline std::vector<uint8_t> buildLut()
{
	std::vector<uint8_t> lut(size_t(256) * 256 * 256 * 3);
	for (uint32_t i = 0; i < (1u << 24); ++i)
	{
		const Rgb c = lutColour(i);
		lut[size_t(i) * 3] = c.r; lut[size_t(i) * 3 + 1] = c.g; lut[size_t(i) * 3 + 2] = c.b;
	}
	return lut;
}

struct Frame
{
	Fmt fmt; int width, height, line;
	std::vector<uint8_t> data;   // exact: every plane, nothing after
	size_t uvOffset() const { return size_t(line) * height; }
	size_t vOffset() const { return uvOffset() + size_t(line / 2) * (height / 2); }   // I420
};

inline size_t frameSize(Fmt f, int line, int h)
{
	switch (f)
	{
		case Fmt::NV12: case Fmt::P010: return size_t(line) * h + size_t(line) * (h / 2);
		case Fmt::I420: return size_t(line) * h + 2 * size_t(line / 2) * (h / 2);
		default: return size_t(line) * h;
	}
}

// Pseudo-random bytes (fixed seed); the bottom-right pixel of every plane is
// all ones, so the last LUT entry (white, index 0xFFFFFF) is read.
inline Frame makeFrame(Fmt f, int w, int h, int pad, uint32_t seed)
{
	Frame fr{ f, w, h, w * bytesPerPixel(f) + pad, {} };
	fr.data.resize(frameSize(f, fr.line, h));
	uint32_t s = seed * 2654435761u + 0x9E3779B9u;
	for (auto& b : fr.data) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; b = uint8_t(s >> 7); }
	auto ones = [&](size_t off, size_t n) { std::memset(&fr.data[off], 0xFF, n); };
	const size_t lastRow = size_t(fr.line) * (h - 1);
	switch (f)
	{
		case Fmt::YUYV: case Fmt::UYVY: ones(lastRow + size_t(w - 2) * 2, 4); break;
		case Fmt::NV12: ones(lastRow + w - 1, 1); ones(fr.uvOffset() + size_t(fr.line) * (h / 2 - 1) + w - 2, 2); break;
		case Fmt::I420:
			ones(lastRow + w - 1, 1);
			ones(fr.uvOffset() + size_t(fr.line / 2) * (h / 2 - 1) + (w / 2 - 1), 1);
			ones(fr.vOffset() + size_t(fr.line / 2) * (h / 2 - 1) + (w / 2 - 1), 1);
			break;
		case Fmt::P010: ones(lastRow + size_t(w - 1) * 2, 2); ones(fr.uvOffset() + size_t(fr.line) * (h / 2 - 1) + size_t(w - 2) * 2, 4); break;
		case Fmt::RGB24: ones(lastRow + size_t(w - 1) * 3, 3); break;
		case Fmt::XRGB: ones(lastRow + size_t(w - 1) * 4, 4); break;
	}
	return fr;
}

struct Opts { bool quarter, tone; int cropLeft, cropRight, cropTop, cropBottom; };

struct Geometry { int cl, cr, ct, cb, step, outW, outH; };
inline Geometry geometry(const Frame& fr, const Opts& o)
{
	Geometry g{ (o.cropLeft >> 1) << 1, (o.cropRight >> 1) << 1, o.cropTop, o.cropBottom, o.quarter ? 2 : 1, 0, 0 };
	if (o.quarter)
		g.cl = g.cr = g.ct = g.cb = 0;
	g.outW = (fr.width - g.cl - g.cr) >> (o.quarter ? 1 : 0);
	g.outH = (fr.height - g.ct - g.cb) >> (o.quarter ? 1 : 0);
	return g;
}

struct P010Tables { const std::array<uint8_t, 1024>* y; const std::array<uint8_t, 1024>* uv; };

inline Rgb expectedPixel(const Frame& fr, const Opts& o, const Geometry& g, int x, int y, const P010Tables& p010)
{
	const uint8_t* d = fr.data.data();
	const int col = g.cl + x * g.step;
	if (isRgb(fr.fmt))
	{
		const int row = fr.height - g.cb - 1 - y * g.step;
		const uint8_t* p = d + size_t(fr.line) * row + size_t(col) * bytesPerPixel(fr.fmt);
		if (!o.tone)
			return { p[2], p[1], p[0] };
		return lutColour(uint32_t(p[2]) | uint32_t(p[1]) << 8 | uint32_t(p[0]) << 16);
	}
	const int row = g.ct + y * g.step;
	uint32_t Y = 0, U = 0, V = 0;
	switch (fr.fmt)
	{
		case Fmt::YUYV: { const uint8_t* m = d + size_t(fr.line) * row + size_t(col / 2) * 4; Y = m[(col & 1) * 2]; U = m[1]; V = m[3]; break; }
		case Fmt::UYVY: { const uint8_t* m = d + size_t(fr.line) * row + size_t(col / 2) * 4; U = m[0]; Y = m[1 + (col & 1) * 2]; V = m[2]; break; }
		case Fmt::NV12:
		{
			Y = d[size_t(fr.line) * row + col];
			const uint8_t* uv = d + fr.uvOffset() + size_t(fr.line) * (row / 2) + size_t(col / 2) * 2;
			U = uv[0]; V = uv[1];
			break;
		}
		case Fmt::I420:
			Y = d[size_t(fr.line) * row + col];
			U = d[fr.uvOffset() + size_t(fr.line / 2) * (row / 2) + col / 2];
			V = d[fr.vOffset() + size_t(fr.line / 2) * (row / 2) + col / 2];
			break;
		case Fmt::P010:
		{
			auto sample = [&](size_t off) { return uint32_t(d[off]) | uint32_t(d[off + 1]) << 8; };
			const uint32_t ys = sample(size_t(fr.line) * row + size_t(col) * 2);
			const size_t uv = fr.uvOffset() + size_t(fr.line) * (row / 2) + size_t(col / 2) * 4;
			const uint32_t us = sample(uv), vs = sample(uv + 2);
			if (o.tone) { Y = (*p010.y)[ys >> 6]; U = (*p010.uv)[us >> 6]; V = (*p010.uv)[vs >> 6]; }
			else { Y = ys >> 8; U = us >> 8; V = vs >> 8; }
			break;
		}
		default: break;
	}
	return lutColour(Y | U << 8 | V << 16);
}

inline std::vector<uint8_t> expected(const Frame& fr, const Opts& o, const P010Tables& p010)
{
	const Geometry g = geometry(fr, o);
	std::vector<uint8_t> out(size_t(g.outW) * g.outH * 3);
	for (int y = 0; y < g.outH; ++y)
		for (int x = 0; x < g.outW; ++x)
		{
			const Rgb c = expectedPixel(fr, o, g, x, y, p010);
			uint8_t* p = &out[(size_t(y) * g.outW + x) * 3];
			p[0] = c.r; p[1] = c.g; p[2] = c.b;
		}
	return out;
}

struct Case { Fmt fmt; int width, height, pad; Opts opts; };

// Widths: packed and planar YUV need even widths (2 pixels share chroma); RGB
// takes any. Output widths include every remainder modulo 4 (the YUV decoders
// emit 4 pixels per step, 2 in quarter mode) so a row's last step is partial.
inline std::vector<Case> cases()
{
	std::vector<Case> out;
	const Fmt fmts[] = { Fmt::YUYV, Fmt::UYVY, Fmt::NV12, Fmt::I420, Fmt::P010, Fmt::RGB24, Fmt::XRGB };
	const int crops[][4] = { { 0, 0, 0, 0 }, { 2, 0, 0, 0 }, { 4, 0, 1, 1 }, { 0, 2, 2, 0 }, { 2, 4, 0, 2 }, { 3, 1, 0, 0 } };
	for (Fmt f : fmts)
	{
		const std::vector<int> widths = isRgb(f) ? std::vector<int>{ 1, 2, 3, 5, 6, 7, 9, 12, 13 } : std::vector<int>{ 2, 4, 6, 8, 10, 14, 18 };
		const std::vector<int> heights = isPlanar(f) ? std::vector<int>{ 2, 4, 6 } : std::vector<int>{ 1, 2, 3, 5 };
		for (bool quarter : { false, true })
			for (bool tone : { false, true })
				for (int w : widths)
					for (int h : heights)
						for (int pad : { 0, 6 })
							for (const auto& c : crops)
							{
								if (quarter && (c[0] || c[1] || c[2] || c[3]))
									continue;   // quarter mode ignores crops
								Case k{ f, w, h, pad, { quarter, tone, c[0], c[1], c[2], c[3] } };
								Frame probe{ f, w, h, w * bytesPerPixel(f) + pad, {} };
								const Geometry g = geometry(probe, k.opts);
								if (g.outW <= 0 || g.outH <= 0)
									continue;
								out.push_back(k);
							}
	}
	return out;
}

inline std::string label(const Case& c)
{
	char s[160];
	std::snprintf(s, sizeof(s), "%s %dx%d line+%d crop L%d R%d T%d B%d%s%s", name(c.fmt), c.width, c.height, c.pad,
		c.opts.cropLeft, c.opts.cropRight, c.opts.cropTop, c.opts.cropBottom, c.opts.quarter ? " quarter" : "", c.opts.tone ? " tone-mapped" : "");
	return s;
}
}
