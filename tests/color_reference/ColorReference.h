#pragma once
// Backend-independent colour-quality references for BGR24 capture (issues #10, #12).
//
// A test builds a source frame from *nominal* full-range colours, encodes it as the
// receiver would send it (full or limited range, BGR byte order, top-down rows,
// padded stride), computes the expected RGB output from a written geometry and
// range specification, and compares any backend's output against it with an
// explicit tolerance. Nothing here calls the code under test, so the same
// references can later judge an RGA or GPU backend next to the CPU decoder.
//
// Geometry specification (what HyperHDR's BGR24 path is required to do):
//   * left and right crop are rounded down to even; top and bottom are not;
//   * quarter output ignores all crop, halves width and height (rounding down) and
//     samples source pixel (2x, 2y): no averaging;
//   * output pixel (x, y) of a full-size frame is source (x + left, y + top);
//   * rows are top-down: source row 0 is output row 0.
// Range specification: limited code c maps to round((c - 16) * 255 / 219), clamped
// to 0..255, computed here in exact integer arithmetic.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace ColorReference
{
struct Rgb { uint8_t r = 0, g = 0, b = 0; bool operator==(const Rgb&) const = default; };

// The reference palette: black, white, mid-gray and the primaries and secondaries.
inline const std::array<std::pair<const char*, Rgb>, 9> palette{ {
	{ "black",   {   0,   0,   0 } }, { "white",   { 255, 255, 255 } }, { "gray",    { 128, 128, 128 } },
	{ "red",     { 255,   0,   0 } }, { "green",   {   0, 255,   0 } }, { "blue",    {   0,   0, 255 } },
	{ "cyan",    {   0, 255, 255 } }, { "magenta", { 255,   0, 255 } }, { "yellow",  { 255, 255,   0 } },
} };

// ---------------------------------------------------------------- range coding

// Full value -> studio-swing code, rounded to nearest: 0 -> 16, 255 -> 235.
constexpr uint8_t encodeLimited(uint8_t v) { return uint8_t(16 + (v * 219 + 127) / 255); }

// Studio-swing code -> full value, exact rational rounding (half up), clamped.
constexpr uint8_t decodeLimited(uint8_t c)
{
	const int num = (int(c) - 16) * 255 * 2 + 219, den = 219 * 2;
	const int v = num >= 0 ? num / den : -((-num + den - 1) / den);
	return uint8_t(std::clamp(v, 0, 255));
}

// ---------------------------------------------------------------- frames

struct Frame
{
	int width = 0, height = 0, stride = 0;
	bool limited = false;
	std::vector<Rgb> nominal;     // the full-range colour each pixel stands for
	std::vector<uint8_t> bytes;   // as sent: BGR, top-down, exactly (height-1)*stride + width*3 bytes
};

// Builds a frame from a nominal colour per pixel. Padding bytes between rows are
// filled with 0xA5 so a stride error shows up as a wrong colour.
inline Frame makeFrame(int width, int height, int stride, bool limited, const std::function<Rgb(int, int)>& colour)
{
	Frame f;
	f.width = width; f.height = height; f.stride = stride; f.limited = limited;
	f.nominal.resize(size_t(width) * height);
	f.bytes.assign(size_t(height - 1) * stride + size_t(width) * 3, 0xA5);   // no slack after the last pixel
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			const Rgb c = colour(x, y);
			f.nominal[size_t(y) * width + x] = c;
			uint8_t* p = &f.bytes[size_t(y) * stride + size_t(x) * 3];
			p[0] = limited ? encodeLimited(c.b) : c.b;
			p[1] = limited ? encodeLimited(c.g) : c.g;
			p[2] = limited ? encodeLimited(c.r) : c.r;
		}
	return f;
}

// Palette cells indexed by (x + 4y) mod 9: a vertical flip, a mirrored row or a
// one-pixel shift changes the colour at most positions.
inline Frame paletteFrame(int width, int height, int stride, bool limited)
{
	return makeFrame(width, height, stride, limited, [](int x, int y) { return palette[size_t(x + 4 * y) % palette.size()].second; });
}

// Every pixel distinct within 256x256, all code values on each channel.
inline Frame rampFrame(int width, int height, int stride, bool limited)
{
	return makeFrame(width, height, stride, limited, [](int x, int y) {
		return Rgb{ uint8_t((x * 7 + y * 13) & 255), uint8_t(y & 255), uint8_t(x & 255) };
	});
}

// Raw transmitted codes 0..255 on every channel, including sub-black (< 16) and
// super-white (> 235) codes a real source can send in limited range. The nominal
// colour is what the specification says those codes mean (clipped for limited).
inline Frame codeFrame(int width, int height, int stride, bool limited)
{
	Frame f;
	f.width = width; f.height = height; f.stride = stride; f.limited = limited;
	f.nominal.resize(size_t(width) * height);
	f.bytes.assign(size_t(height - 1) * stride + size_t(width) * 3, 0xA5);
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			const uint8_t b = uint8_t(x + y * width), g = uint8_t(255 - b), r = uint8_t(b * 7 + 3);
			uint8_t* p = &f.bytes[size_t(y) * stride + size_t(x) * 3];
			p[0] = b; p[1] = g; p[2] = r;
			f.nominal[size_t(y) * width + x] = limited ? Rgb{ decodeLimited(r), decodeLimited(g), decodeLimited(b) } : Rgb{ r, g, b };
		}
	return f;
}

// ---------------------------------------------------------------- expected output

struct Geometry
{
	int cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
	bool quarter = false;
};

// A transform applied by a lookup table, in output (RGB) terms.
using Transform = std::function<Rgb(Rgb)>;

struct Image
{
	int width = 0, height = 0;
	std::vector<Rgb> pixels;
	Rgb at(int x, int y) const { return pixels[size_t(y) * width + x]; }
};

// What the frame should decode to, from the specification above. `fromCodes`
// decodes the transmitted codes (exact reference); otherwise the nominal colours
// are used (what the source meant, which limited range can only approximate).
inline Image expected(const Frame& f, const Geometry& g, const Transform& lut = {}, bool fromCodes = true)
{
	int left = (g.cropLeft / 2) * 2, right = (g.cropRight / 2) * 2, top = g.cropTop, bottom = g.cropBottom;
	if (g.quarter) left = right = top = bottom = 0;
	const int step = g.quarter ? 2 : 1;
	Image out;
	out.width = (f.width - left - right) / step;
	out.height = (f.height - top - bottom) / step;
	for (int y = 0; y < out.height; ++y)
		for (int x = 0; x < out.width; ++x)
		{
			const int sx = x * step + left, sy = y * step + top;
			Rgb c;
			if (fromCodes)
			{
				const uint8_t* p = &f.bytes[size_t(sy) * f.stride + size_t(sx) * 3];
				c = f.limited ? Rgb{ decodeLimited(p[2]), decodeLimited(p[1]), decodeLimited(p[0]) } : Rgb{ p[2], p[1], p[0] };
			}
			else
				c = f.nominal[size_t(sy) * f.width + sx];
			out.pixels.push_back(lut ? lut(c) : c);
		}
	return out;
}

// LUT in HyperHDR's layout: 2^24 entries of 3 bytes, index (r | g<<8 | b<<16) * 3,
// written in file order (blue outermost, red innermost).
inline std::vector<uint8_t> makeLut(const Transform& t)
{
	std::vector<uint8_t> lut(size_t(256) * 256 * 256 * 3);
	size_t i = 0;
	for (int b = 0; b < 256; ++b)
		for (int g = 0; g < 256; ++g)
			for (int r = 0; r < 256; ++r)
			{
				const Rgb o = t(Rgb{ uint8_t(r), uint8_t(g), uint8_t(b) });
				lut[i++] = o.r; lut[i++] = o.g; lut[i++] = o.b;
			}
	return lut;
}

// ---------------------------------------------------------------- comparison

struct Diff
{
	bool sizeMatches = true;
	std::array<int, 3> maxError{ 0, 0, 0 };   // R, G, B
	size_t overTolerance = 0;
	int firstX = -1, firstY = -1;             // first pixel over tolerance
	Rgb firstExpected, firstActual;
	bool ok() const { return sizeMatches && overTolerance == 0; }
	std::string describe() const
	{
		if (!sizeMatches) return "output size differs";
		std::string s = "max |error| R/G/B = " + std::to_string(maxError[0]) + "/" + std::to_string(maxError[1]) + "/" + std::to_string(maxError[2]);
		if (overTolerance)
			s += ", " + std::to_string(overTolerance) + " pixel(s) over tolerance, first at (" + std::to_string(firstX) + "," + std::to_string(firstY) +
				") expected " + std::to_string(firstExpected.r) + "," + std::to_string(firstExpected.g) + "," + std::to_string(firstExpected.b) +
				" got " + std::to_string(firstActual.r) + "," + std::to_string(firstActual.g) + "," + std::to_string(firstActual.b);
		return s;
	}
};

// Compares packed RGB output (3 bytes per pixel, rows of width*3 bytes) with the
// expectation. Tolerance is per channel, in code values.
inline Diff compare(const Image& want, int width, int height, const uint8_t* rgb, int tolerance)
{
	Diff d;
	if (width != want.width || height != want.height) { d.sizeMatches = false; return d; }
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			const Rgb e = want.at(x, y);
			const uint8_t* p = rgb + (size_t(y) * width + x) * 3;
			const int err[3] = { std::abs(p[0] - e.r), std::abs(p[1] - e.g), std::abs(p[2] - e.b) };
			bool over = false;
			for (int c = 0; c < 3; ++c) { d.maxError[c] = std::max(d.maxError[c], err[c]); over |= err[c] > tolerance; }
			if (over && d.overTolerance++ == 0) { d.firstX = x; d.firstY = y; d.firstExpected = e; d.firstActual = { p[0], p[1], p[2] }; }
		}
	return d;
}

// ---------------------------------------------------------------- cases

// One decode to check: a frame, a geometry, an optional LUT transform, and the
// tolerance against (a) the exact code reference and (b) the nominal colours.
struct Case
{
	std::string name;
	Frame frame;
	Geometry geometry;
	std::string lutName;   // "" = no LUT
	Transform lut;
	int toleranceExact = 0;     // vs decoded codes: the CPU path is exact
	int toleranceNominal = 0;   // vs what the source meant: limited range can be off by 1
};

// The standard set. A backend decodes each case and is compared twice.
inline std::vector<Case> standardCases()
{
	std::vector<Case> cases;
	const Transform identity = [](Rgb c) { return c; };
	const Transform invert = [](Rgb c) { return Rgb{ uint8_t(255 - c.r), uint8_t(255 - c.g), uint8_t(255 - c.b) }; };
	const Transform rotate = [](Rgb c) { return Rgb{ c.g, c.b, c.r }; };   // channel permutation: catches swapped LUT bytes
	for (bool limited : { false, true })
	{
		const std::string range = limited ? "limited" : "full";
		const int nominalTol = limited ? 1 : 0;   // round trip through studio swing: <= 1 code (tests/bgr24 README)
		auto add = [&](std::string name, Frame f, Geometry g, std::string lutName = {}, Transform t = {}) {
			cases.push_back({ range + " " + name, std::move(f), g, std::move(lutName), std::move(t), 0, nominalTol });
		};
		add("palette 9x5 tight stride", paletteFrame(9, 5, 27, limited), {});
		add("palette 9x5 padded stride", paletteFrame(9, 5, 32, limited), {});
		add("palette 7x5 odd dims quarter", paletteFrame(7, 5, 24, limited), { 0, 0, 0, 0, true });
		add("palette 8x6 quarter ignores crop", paletteFrame(8, 6, 24, limited), { 2, 2, 1, 1, true });
		add("palette 11x7 odd crop L3 R1 T1 B2", paletteFrame(11, 7, 36, limited), { 3, 1, 1, 2, false });
		add("palette 10x6 right/bottom crop only", paletteFrame(10, 6, 30, limited), { 0, 2, 0, 3, false });
		add("palette 1x1", paletteFrame(1, 1, 3, limited), {});
		add("palette 3x3 quarter -> 1x1", paletteFrame(3, 3, 9, limited), { 0, 0, 0, 0, true });
		add("ramp 256x4 all nominal values", rampFrame(256, 4, 768 + 6, limited), {});
		add("ramp 33x9 quarter odd", rampFrame(33, 9, 100, limited), { 0, 0, 0, 0, true });
		add("codes 0..255 incl. sub-black/super-white", codeFrame(16, 16, 51, limited), {});
		add("codes 0..255 LUT invert quarter", codeFrame(32, 16, 99, limited), { 0, 0, 0, 0, true }, "invert", invert);
		add("palette 9x5 LUT identity", paletteFrame(9, 5, 30, limited), {}, "identity", identity);
		add("ramp 64x8 LUT invert", rampFrame(64, 8, 192, limited), {}, "invert", invert);
		add("palette 7x5 LUT rotate quarter", paletteFrame(7, 5, 24, limited), { 0, 0, 0, 0, true }, "rotate", rotate);
		add("ramp 21x9 LUT rotate crop L2 T3", rampFrame(21, 9, 66, limited), { 2, 0, 3, 0, false }, "rotate", rotate);
	}
	return cases;
}

// A decoder under test: fills `rgb` (packed, rows of width*3) and its size.
// Returns false if it can't handle the case (reported, not treated as a pass).
using Backend = std::function<bool(const Case&, const uint8_t* lut, std::vector<uint8_t>& rgb, int& width, int& height)>;

struct Result { std::string name; bool ran = false; Diff exact, nominal; };

// Runs every case through a backend. LUTs are built once per transform name.
inline std::vector<Result> run(const std::vector<Case>& cases, const Backend& backend)
{
	std::vector<Result> results;
	std::string lutName;
	std::vector<uint8_t> lut;
	for (const auto& c : cases)
	{
		if (c.lut && c.lutName != lutName) { lut = makeLut(c.lut); lutName = c.lutName; }
		Result r;
		r.name = c.name;
		std::vector<uint8_t> rgb;
		int w = 0, h = 0;
		r.ran = backend(c, c.lut ? lut.data() : nullptr, rgb, w, h);
		if (r.ran)
		{
			r.exact = compare(expected(c.frame, c.geometry, c.lut, true), w, h, rgb.data(), c.toleranceExact);
			r.nominal = compare(expected(c.frame, c.geometry, c.lut, false), w, h, rgb.data(), c.toleranceNominal);
		}
		results.push_back(r);
	}
	return results;
}
}
