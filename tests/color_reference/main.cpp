// Qt-free tests for the colour-quality reference framework (ColorReference.h).
//
// 1. The references themselves: range coding against independent formulas, and
//    the expected-output geometry against hand-worked small frames.
// 2. A CPU backend built from Bgr24::row (the production pixel/row code) passes
//    every standard case, exact against the codes and within 1 of nominal.
// 3. Negative controls: deliberately wrong backends (swapped channels, bottom-up
//    rows, ignored stride, averaged quarter, unaligned crop, LUT index or range
//    applied in the wrong order) must each fail, so the references can tell a
//    correct decoder from a plausible wrong one.
//
// The real FrameDecoder dispatch runs the same cases in tests/codec_test
// (`DecoderTest --color-quality`).
#include "ColorReference.h"
#include <utils/Bgr24.h>

#include <algorithm>
#include <cmath>
#include <iostream>

using namespace ColorReference;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " #cond "\n"; } } while (0)

static void rangeCoding()
{
	CHECK(encodeLimited(0) == 16 && encodeLimited(255) == 235 && encodeLimited(128) == 126);
	CHECK(decodeLimited(16) == 0 && decodeLimited(235) == 255 && decodeLimited(126) == 128);
	CHECK(decodeLimited(0) == 0 && decodeLimited(15) == 0 && decodeLimited(240) == 255 && decodeLimited(255) == 255);
	for (int c = 0; c < 256; ++c)
	{
		// Independent: floating point, round half up, clamp.
		const int f = std::clamp(int(std::floor((c - 16) * 255.0 / 219.0 + 0.5)), 0, 255);
		CHECK(decodeLimited(uint8_t(c)) == f);
	}
	for (int v = 0; v < 256; ++v)
	{
		const int e = encodeLimited(uint8_t(v));
		CHECK(e == int(std::floor(16 + v * 219.0 / 255.0 + 0.5)));
		CHECK(std::abs(decodeLimited(uint8_t(e)) - v) <= 1);   // the tolerance limited cases use against nominal
	}
	// Primaries and secondaries survive the round trip exactly (only 0 and 255 occur).
	for (const auto& [name, c] : palette)
		if (std::string(name) != "gray")
			CHECK(decodeLimited(encodeLimited(c.r)) == c.r && decodeLimited(encodeLimited(c.g)) == c.g && decodeLimited(encodeLimited(c.b)) == c.b);
}

static void frames()
{
	const Frame f = paletteFrame(3, 2, 11, false);
	CHECK(f.bytes.size() == 11 + 9);   // exact: (h-1)*stride + w*3, no slack
	// (0,0) black, (1,0) white, (2,0) gray; (0,1) index 4 = green; BGR byte order.
	CHECK(f.bytes[3] == 255 && f.bytes[6] == 128);
	CHECK(f.bytes[11] == 0 && f.bytes[12] == 255 && f.bytes[13] == 0);
	CHECK(f.bytes[9] == 0xA5 && f.bytes[10] == 0xA5);   // row padding
	const Frame l = paletteFrame(4, 1, 12, true);
	CHECK(l.bytes[0] == 16 && l.bytes[3] == 235 && l.bytes[6] == 126);   // black, white, gray in studio swing
	CHECK(l.bytes[9] == 16 && l.bytes[10] == 16 && l.bytes[11] == 235);  // red: B=16, G=16, R=235
	// Raw codes: every code value 0..255 appears; limited nominal clips sub-black and super-white.
	const Frame c = codeFrame(16, 16, 48, true);
	std::array<bool, 256> seen{};
	for (int i = 0; i < 256; ++i) seen[c.bytes[size_t(i / 16) * 48 + size_t(i % 16) * 3]] = true;
	CHECK(std::all_of(seen.begin(), seen.end(), [](bool b) { return b; }));
	CHECK(c.nominal[0].b == 0 && c.nominal[10].b == 0 && c.nominal[240].b == 255 && c.nominal[255].b == 255);
}

static void geometry()
{
	// 5x3 ramp: pixel (x,y) has B=x, G=y.
	const Frame f = rampFrame(5, 3, 16, false);
	auto e = expected(f, { 0, 0, 0, 0, true });
	CHECK(e.width == 2 && e.height == 1 && e.at(0, 0).b == 0 && e.at(1, 0).b == 2);   // samples (0,0), (2,0)
	e = expected(f, { 3, 1, 1, 0, false });   // left 3 -> 2, right 1 -> 0, top 1
	CHECK(e.width == 3 && e.height == 2 && e.at(0, 0).b == 2 && e.at(0, 0).g == 1 && e.at(2, 1).b == 4 && e.at(2, 1).g == 2);
	e = expected(f, { 2, 2, 1, 1, true });    // quarter ignores crop
	CHECK(e.width == 2 && e.height == 1 && e.at(0, 0).b == 0 && e.at(0, 0).g == 0);
	e = expected(f, { 0, 2, 0, 1, false });   // right/bottom crop keep the origin
	CHECK(e.width == 3 && e.height == 2 && e.at(0, 0).b == 0 && e.at(2, 1).b == 2 && e.at(2, 1).g == 1);
	// Limited: exact reference decodes the codes; nominal keeps the source's intent.
	const Frame g = makeFrame(1, 1, 3, true, [](int, int) { return Rgb{ 1, 128, 254 }; });
	CHECK((expected(g, {}).at(0, 0) == Rgb{ decodeLimited(encodeLimited(1)), 128, decodeLimited(encodeLimited(254)) }));
	CHECK((expected(g, {}, {}, false).at(0, 0) == Rgb{ 1, 128, 254 }));
}

static void comparison()
{
	Image want{ 2, 1, { { 10, 20, 30 }, { 40, 50, 60 } } };
	std::vector<uint8_t> got = { 10, 20, 30, 41, 50, 58 };
	auto d = compare(want, 2, 1, got.data(), 0);
	CHECK(!d.ok() && d.overTolerance == 1 && d.firstX == 1 && d.maxError[0] == 1 && d.maxError[2] == 2);
	CHECK(!compare(want, 2, 1, got.data(), 1).ok() && compare(want, 2, 1, got.data(), 2).ok());
	CHECK(!compare(want, 1, 2, got.data(), 255).ok());   // wrong size never passes
}

// ---- backends ---------------------------------------------------------------

struct Faults
{
	bool swapRB = false, bottomUp = false, ignoreStride = false, averageQuarter = false, unalignedCrop = false,
		lutBgrIndex = false, lutBeforeRange = false, noRange = false;
};

// The production row code (Bgr24::row) driven by the specified geometry, with
// optional injected faults for the negative controls.
static Backend cpuBackend(Faults fault = {})
{
	return [fault](const Case& c, const uint8_t* lut, std::vector<uint8_t>& rgb, int& w, int& h) {
		const Frame& f = c.frame;
		Geometry g = c.geometry;
		int left = fault.unalignedCrop ? g.cropLeft : (g.cropLeft / 2) * 2, right = (g.cropRight / 2) * 2, top = g.cropTop, bottom = g.cropBottom;
		if (g.quarter) left = right = top = bottom = 0;
		const int step = g.quarter ? 2 : 1;
		w = (f.width - left - right) / step;
		h = (f.height - top - bottom) / step;
		rgb.assign(size_t(w) * h * 3, 0);
		const bool limited = f.limited && !fault.noRange;
		const int stride = fault.ignoreStride ? f.width * 3 : f.stride;
		for (int y = 0; y < h; ++y)
		{
			const int sy = fault.bottomUp ? (f.height - 1 - bottom) - y * step : top + y * step;
			const uint8_t* src = f.bytes.data() + size_t(sy) * stride + size_t(left) * 3;
			uint8_t* dst = rgb.data() + size_t(y) * w * 3;
			if (fault.averageQuarter && g.quarter)
			{
				for (int x = 0; x < w; ++x)
					for (int k = 0; k < 3; ++k)
					{
						const int a = src[x * 6 + k], b = (2 * x + 1 < f.width) ? src[x * 6 + 3 + k] : a;
						dst[x * 3 + 2 - k] = uint8_t((a + b + 1) / 2);
					}
			}
			else if (lut && (fault.lutBgrIndex || fault.lutBeforeRange))
			{
				for (int x = 0; x < w; ++x)
				{
					const uint8_t* s = src + size_t(x) * step * 3;
					uint8_t r = s[2], gg = s[1], b = s[0];
					if (!fault.lutBeforeRange && limited) { r = Bgr24::limitedTable[r]; gg = Bgr24::limitedTable[gg]; b = Bgr24::limitedTable[b]; }
					const uint32_t index = fault.lutBgrIndex ? (uint32_t(b) | (uint32_t(gg) << 8) | (uint32_t(r) << 16)) * 3
					                                         : (uint32_t(r) | (uint32_t(gg) << 8) | (uint32_t(b) << 16)) * 3;
					for (int k = 0; k < 3; ++k)
						dst[x * 3 + k] = (fault.lutBeforeRange && limited) ? Bgr24::limitedTable[lut[index + k]] : lut[index + k];
				}
			}
			else if (step == 2)
			{
				if (lut) (limited ? Bgr24::row<true, true, 2> : Bgr24::row<true, false, 2>)(src, dst, w, lut);
				else (limited ? Bgr24::row<false, true, 2> : Bgr24::row<false, false, 2>)(src, dst, w, nullptr);
			}
			else
			{
				if (lut) (limited ? Bgr24::row<true, true, 1> : Bgr24::row<true, false, 1>)(src, dst, w, lut);
				else (limited ? Bgr24::row<false, true, 1> : Bgr24::row<false, false, 1>)(src, dst, w, nullptr);
			}
			if (fault.swapRB)
				for (int x = 0; x < w; ++x) std::swap(dst[x * 3], dst[x * 3 + 2]);
		}
		return true;
	};
}

static bool allPass(const std::vector<Result>& rs, bool print)
{
	bool ok = true;
	for (const auto& r : rs)
	{
		const bool pass = r.ran && r.exact.ok() && r.nominal.ok();
		ok &= pass;
		if (print || !pass)
			std::cout << (pass ? "PASS " : "FAIL ") << r.name << ": exact " << r.exact.describe() << "; nominal " << r.nominal.describe() << "\n";
	}
	return ok;
}

static void backends()
{
	const auto cases = standardCases();
	CHECK(cases.size() == 32);
	std::cout << "-- CPU backend (Bgr24::row)\n";
	CHECK(allPass(run(cases, cpuBackend()), true));

	struct Control { const char* name; Faults f; };
	const Control controls[] = {
		{ "R and B swapped", { .swapRB = true } },
		{ "bottom-up rows", { .bottomUp = true } },
		{ "stride ignored", { .ignoreStride = true } },
		{ "quarter averages pairs", { .averageQuarter = true } },
		{ "odd left crop not aligned", { .unalignedCrop = true } },
		{ "LUT indexed b|g<<8|r<<16", { .lutBgrIndex = true } },
		{ "range expanded after the LUT", { .lutBeforeRange = true } },
		{ "limited treated as full", { .noRange = true } },
	};
	for (const auto& c : controls)
	{
		size_t failed = 0;
		for (const auto& r : run(cases, cpuBackend(c.f))) failed += !(r.exact.ok() && r.nominal.ok());
		std::cout << (failed ? "PASS " : "FAIL ") << "negative control '" << c.name << "' is detected (" << failed << " case(s) fail)\n";
		CHECK(failed > 0);
	}
}

int main()
{
	rangeCoding();
	frames();
	geometry();
	comparison();
	backends();
	if (failures) { std::cerr << failures << " check(s) failed\n"; return 1; }
	std::cout << "PASS colour reference framework: range coding, frames, geometry, comparison, CPU backend, 8 negative controls\n";
	return 0;
}
