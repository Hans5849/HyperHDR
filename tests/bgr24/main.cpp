// Independent reference tests for the BGR24 conversion in include/utils/Bgr24.h
// (issues #10, #12). Hardware-free and Qt-free.
//
// The expected values here are NOT produced by Bgr24::expandLimited or by the
// code under test: limited-range expansion is recomputed in exact integer
// rational arithmetic and in floating point, and the LUT is built in its on-disk
// order (blue outer, green, red inner, three bytes per entry) and then read back
// through the decoder's index arithmetic.
#include <utils/Bgr24.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int failures = 0;
#define REQUIRE(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

namespace
{
// ---- independent references ------------------------------------------------
// Studio-swing (16..235) to full range, rounded half up, clamped.
int refExactRational(int v)
{
	if (v <= 16) return 0;
	if (v >= 235) return 255;
	return ((v - 16) * 255 * 2 + 219) / (219 * 2);   // floor(n*255/219 + 1/2)
}
double refReal(int v)   // the ideal, unrounded value
{
	return std::clamp((v - 16) * 255.0 / 219.0, 0.0, 255.0);
}
int refFloat(int v) { return static_cast<int>(std::floor(refReal(v) + 0.5)); }

// Deterministic generator (xorshift32): no <random> implementation differences.
struct Rng
{
	uint32_t s = 0x9e3779b9u;
	uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
	uint8_t byte() { return static_cast<uint8_t>(next() >> 24); }
};

// What the synthetic LUT holds for a given (r, g, b) and output channel k.
uint8_t lutValue(int r, int g, int b, int k)
{
	uint32_t h = static_cast<uint32_t>(r) * 73856093u ^ static_cast<uint32_t>(g) * 19349663u ^ static_cast<uint32_t>(b) * 83492791u;
	h ^= static_cast<uint32_t>(k + 1) * 2654435761u;
	return static_cast<uint8_t>(h >> 13);
}

constexpr size_t lutBytes = size_t(256) * 256 * 256 * 3;

// Build in file order: for b, for g, for r, then the three output bytes.
template<typename Fn>
std::vector<uint8_t> makeLut(Fn value)
{
	std::vector<uint8_t> lut(lutBytes);
	size_t at = 0;
	for (int b = 0; b < 256; ++b)
		for (int g = 0; g < 256; ++g)
			for (int r = 0; r < 256; ++r)
				for (int k = 0; k < 3; ++k) lut[at++] = value(r, g, b, k);
	return lut;
}

// Expected output pixel (R, G, B) for BGR source bytes, straight from the spec.
void expected(const uint8_t* bgr, bool limited, int out[3])
{
	const int b = limited ? refExactRational(bgr[0]) : bgr[0];
	const int g = limited ? refExactRational(bgr[1]) : bgr[1];
	const int r = limited ? refExactRational(bgr[2]) : bgr[2];
	out[0] = r; out[1] = g; out[2] = b;
}

// ---- tests -----------------------------------------------------------------
void testLimitedRangeAgainstReferences()
{
	int worstIndex = 0; double worstError = 0;
	for (int v = 0; v < 256; ++v)
	{
		REQUIRE(refExactRational(v) == refFloat(v));                  // the two references agree
		REQUIRE(Bgr24::expandLimited(uint8_t(v)) == refExactRational(v));
		REQUIRE(Bgr24::limitedTable[v] == refExactRational(v));       // table matches too
		const double err = std::fabs(refReal(v) - Bgr24::limitedTable[v]);
		if (err > worstError) { worstError = err; worstIndex = v; }
	}
	// Documented numerical difference: rounding to the nearest integer, so the
	// output is within half a code value of the ideal real-valued mapping.
	REQUIRE(worstError <= 0.5 + 1e-9);
	std::printf("limited->full: worst error vs ideal %.4f at input %d (bound 0.5)\n", worstError, worstIndex);

	// Clipping boundaries: super-black and super-white are clamped, not wrapped.
	for (int v = 0; v <= 16; ++v) REQUIRE(Bgr24::limitedTable[v] == 0);
	for (int v = 235; v <= 255; ++v) REQUIRE(Bgr24::limitedTable[v] == 255);
	REQUIRE(Bgr24::limitedTable[17] == 1 && Bgr24::limitedTable[18] == 2 && Bgr24::limitedTable[234] == 254);
	REQUIRE(Bgr24::limitedTable[126] == 128);                          // 110 * 255 / 219 = 128.08

	// 16..235 is 220 codes spread over 256: strictly increasing, steps of 1 or 2.
	for (int v = 16; v < 235; ++v)
	{
		const int step = Bgr24::limitedTable[v + 1] - Bgr24::limitedTable[v];
		REQUIRE(step == 1 || step == 2);
	}
	// Round trip full -> limited -> full stays within one code value.
	int worstRoundTrip = 0;
	for (int x = 0; x < 256; ++x)
	{
		const int limitedCode = int(std::floor(x * 219.0 / 255.0 + 0.5)) + 16;
		worstRoundTrip = std::max(worstRoundTrip, std::abs(x - int(Bgr24::limitedTable[limitedCode])));
	}
	REQUIRE(worstRoundTrip <= 1);
	std::printf("full->limited->full: worst round-trip error %d code value(s) (bound 1)\n", worstRoundTrip);
}

void testChannelOrderAndReferenceColors()
{
	struct Color { const char* name; uint8_t bgr[3]; uint8_t rgbFull[3]; };
	// Source bytes are B, G, R (V4L2 BGR3); output pixels are R, G, B.
	const Color colors[] = {
		{"black",   {0, 0, 0},       {0, 0, 0}},
		{"white",   {255, 255, 255}, {255, 255, 255}},
		{"red",     {0, 0, 255},     {255, 0, 0}},
		{"green",   {0, 255, 0},     {0, 255, 0}},
		{"blue",    {255, 0, 0},     {0, 0, 255}},
		{"yellow",  {0, 255, 255},   {255, 255, 0}},
		{"cyan",    {255, 255, 0},   {0, 255, 255}},
		{"magenta", {255, 0, 255},   {255, 0, 255}},
		{"channel ids", {1, 2, 3},   {3, 2, 1}},
	};
	for (const auto& c : colors)
	{
		uint8_t out[3];
		Bgr24::pixel<false, false>(c.bgr, out, nullptr);
		REQUIRE(out[0] == c.rgbFull[0] && out[1] == c.rgbFull[1] && out[2] == c.rgbFull[2]);
		if (out[0] != c.rgbFull[0]) std::fprintf(stderr, "  (%s)\n", c.name);
	}
	// The same colors in studio swing: 16 -> 0, 235 -> 255 per channel.
	const uint8_t studioRed[3] = {16, 16, 235}, studioWhite[3] = {235, 235, 235}, studioBlack[3] = {16, 16, 16};
	uint8_t out[3];
	Bgr24::pixel<false, true>(studioRed, out, nullptr);   REQUIRE(out[0] == 255 && out[1] == 0 && out[2] == 0);
	Bgr24::pixel<false, true>(studioWhite, out, nullptr); REQUIRE(out[0] == 255 && out[1] == 255 && out[2] == 255);
	Bgr24::pixel<false, true>(studioBlack, out, nullptr); REQUIRE(out[0] == 0 && out[1] == 0 && out[2] == 0);
	// Without range expansion studio black and white stay at 16 and 235: the
	// symptom the receiver/format mismatch warning reports.
	Bgr24::pixel<false, false>(studioBlack, out, nullptr); REQUIRE(out[0] == 16);
	Bgr24::pixel<false, false>(studioWhite, out, nullptr); REQUIRE(out[0] == 235);
}

void testGrayRampsAndRuntimeSelection()
{
	for (int v = 0; v < 256; ++v)
	{
		const uint8_t gray[3] = {uint8_t(v), uint8_t(v), uint8_t(v)};
		uint8_t full[3], lim[3], runtimeFull[3], runtimeLim[3];
		Bgr24::pixel<false, false>(gray, full, nullptr);
		Bgr24::pixel<false, true>(gray, lim, nullptr);
		Bgr24::pixel<false>(gray, runtimeFull, false, nullptr);
		Bgr24::pixel<false>(gray, runtimeLim, true, nullptr);
		for (int c = 0; c < 3; ++c)
		{
			REQUIRE(full[c] == v);                                      // full range is the identity
			REQUIRE(lim[c] == refExactRational(v));
			REQUIRE(runtimeFull[c] == full[c] && runtimeLim[c] == lim[c]);   // runtime flag == template choice
		}
	}
	// Every value in every channel position, in both ranges, with the others varying.
	Rng rng;
	for (int v = 0; v < 256; ++v)
		for (int position = 0; position < 3; ++position)
			for (bool limited : {false, true})
			{
				uint8_t src[3] = {rng.byte(), rng.byte(), rng.byte()};
				src[position] = uint8_t(v);
				uint8_t out[3]; int want[3];
				Bgr24::pixel<false>(src, out, limited, nullptr);
				expected(src, limited, want);
				REQUIRE(out[0] == want[0] && out[1] == want[1] && out[2] == want[2]);
			}
}

void testLutIndexLayoutAndBounds()
{
	const auto lut = makeLut(lutValue);
	REQUIRE(lut.size() == lutBytes);
	const auto check = [&](const uint8_t* bgr, bool limited) {
		int want[3]; expected(bgr, limited, want);          // range expansion happens before the LUT
		uint8_t out[3];
		if (limited) Bgr24::pixel<true, true>(bgr, out, lut.data()); else Bgr24::pixel<true, false>(bgr, out, lut.data());
		REQUIRE(out[0] == lutValue(want[0], want[1], want[2], 0));
		REQUIRE(out[1] == lutValue(want[0], want[1], want[2], 1));
		REQUIRE(out[2] == lutValue(want[0], want[1], want[2], 2));
	};
	// Corners and axes. (255,255,255) reads the very last three bytes of an
	// exactly-sized buffer, so an off-by-one read is caught under ASan.
	for (int b : {0, 1, 16, 17, 127, 234, 235, 254, 255})
		for (int g : {0, 16, 128, 235, 255})
			for (int r : {0, 1, 16, 128, 235, 255})
				for (bool limited : {false, true})
				{
					const uint8_t src[3] = {uint8_t(b), uint8_t(g), uint8_t(r)};
					check(src, limited);
				}
	Rng rng;
	for (int i = 0; i < 20000; ++i)
	{
		const uint8_t src[3] = {rng.byte(), rng.byte(), rng.byte()};
		check(src, (i & 1) != 0);
	}
}

void testIdentityLutMatchesDirectPath()
{
	// With an identity LUT the tone-mapped output must equal the direct output, in
	// both ranges and through the row template: LUT on/off must not shift anything.
	const auto lut = makeLut([](int r, int g, int b, int k) { return uint8_t(k == 0 ? r : k == 1 ? g : b); });
	Rng rng;
	for (int i = 0; i < 50000; ++i)
	{
		const uint8_t src[3] = {rng.byte(), rng.byte(), rng.byte()};
		for (bool limited : {false, true})
		{
			uint8_t direct[3], mapped[3];
			Bgr24::pixel<false>(src, direct, limited, nullptr);
			Bgr24::pixel<true>(src, mapped, limited, lut.data());
			REQUIRE(direct[0] == mapped[0] && direct[1] == mapped[1] && direct[2] == mapped[2]);
		}
	}
	std::vector<uint8_t> src(97 * 3); for (auto& b : src) b = rng.byte();
	std::vector<uint8_t> direct(97 * 3), mapped(97 * 3);
	Bgr24::row<false, true, 1>(src.data(), direct.data(), 97, nullptr);
	Bgr24::row<true, true, 1>(src.data(), mapped.data(), 97, lut.data());
	REQUIRE(direct == mapped);
}

template<bool Limited, int Step>
void rowCase(int width, Rng& rng)
{
	// Exactly-sized buffers: the last source pixel is the last three bytes, so any
	// wider read (a four-byte load at the tail) trips ASan. Canaries guard the output.
	const size_t sourceBytes = (size_t(Step) * (width - 1) + 1) * 3;
	std::vector<uint8_t> source(sourceBytes);
	for (auto& b : source) b = rng.byte();
	constexpr uint8_t canary = 0xa5;
	std::vector<uint8_t> dest(size_t(width) * 3 + 16, canary);
	Bgr24::row<false, Limited, Step>(source.data(), dest.data() + 8, width, nullptr);
	for (int x = 0; x < width; ++x)
	{
		int want[3]; expected(&source[size_t(x) * Step * 3], Limited, want);
		const uint8_t* got = &dest[8 + size_t(x) * 3];
		REQUIRE(got[0] == want[0] && got[1] == want[1] && got[2] == want[2]);
	}
	for (int i = 0; i < 8; ++i) REQUIRE(dest[i] == canary && dest[8 + size_t(width) * 3 + i] == canary);
}

void testRowWidthsStepAndTail()
{
	Rng rng;
	// Odd, tiny, vector-unfriendly and common widths; step 2 is the quarter-size output.
	for (int width : {1, 2, 3, 5, 7, 8, 9, 15, 16, 17, 31, 33, 63, 65, 640, 1279, 1280, 1919, 1920, 1921, 3839, 3840})
	{
		rowCase<false, 1>(width, rng); rowCase<true, 1>(width, rng);
		rowCase<false, 2>(width, rng); rowCase<true, 2>(width, rng);
	}
}

void testQuarterSamplingSelectsEvenPixels()
{
	// Step 2 takes source pixels 0, 2, 4 ...: top-left of each 2x2 block, no averaging.
	std::vector<uint8_t> source(8 * 3);
	for (int x = 0; x < 8; ++x) { source[x * 3] = uint8_t(10 * x); source[x * 3 + 1] = uint8_t(10 * x + 1); source[x * 3 + 2] = uint8_t(10 * x + 2); }
	std::vector<uint8_t> dest(4 * 3);
	Bgr24::row<false, false, 2>(source.data(), dest.data(), 4, nullptr);
	for (int x = 0; x < 4; ++x)
		REQUIRE(dest[x * 3] == 20 * x + 2 && dest[x * 3 + 1] == 20 * x + 1 && dest[x * 3 + 2] == 20 * x);
}
}

int main()
{
	testLimitedRangeAgainstReferences();
	testChannelOrderAndReferenceColors();
	testGrayRampsAndRuntimeSelection();
	testLutIndexLayoutAndBounds();
	testIdentityLutMatchesDirectPath();
	testRowWidthsStepAndTail();
	testQuarterSamplingSelectsEvenPixels();
	if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
	std::puts("all BGR24 reference checks passed");
	return 0;
}
