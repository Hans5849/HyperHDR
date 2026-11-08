#pragma once
#include <array>
#include <cstdint>

namespace Bgr24
{
// Reference conversion for limited (16..235) to full range. Used to build the
// lookup table below, and by tests as the expected value.
constexpr uint8_t expandLimited(uint8_t value)
{
	if (value <= 16) return 0;
	if (value >= 235) return 255;
	return static_cast<uint8_t>(((value - 16) * 255 + 109) / 219);
}

// One table lookup per channel instead of an integer division.
inline constexpr std::array<uint8_t, 256> limitedTable = []() {
	std::array<uint8_t, 256> table{};
	for (int i = 0; i < 256; ++i) table[i] = expandLimited(static_cast<uint8_t>(i));
	return table;
}();

// Exactly three bytes read/written: safe at the last pixel of an MMAP plane.
// Conversion, range expansion and optional LUT happen in the output pass.
template<bool ToneMapping, bool Limited>
inline void pixel(const uint8_t* source, uint8_t* dest, const uint8_t* lut)
{
	const uint8_t r = Limited ? limitedTable[source[2]] : source[2];
	const uint8_t g = Limited ? limitedTable[source[1]] : source[1];
	const uint8_t b = Limited ? limitedTable[source[0]] : source[0];
	if constexpr (ToneMapping)
	{
		const auto index = (uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16)) * 3;
		dest[0] = lut[index]; dest[1] = lut[index + 1]; dest[2] = lut[index + 2];
	}
	else { dest[0] = r; dest[1] = g; dest[2] = b; }
}

template<bool ToneMapping>
inline void pixel(const uint8_t* source, uint8_t* dest, bool limited, const uint8_t* lut)
{
	if (limited) pixel<ToneMapping, true>(source, dest, lut);
	else pixel<ToneMapping, false>(source, dest, lut);
}

// Convert one output row. Range and scaling are compile-time so the inner loop
// carries no per-pixel branches. Step is the source pixel step (1 or 2).
template<bool ToneMapping, bool Limited, int Step>
inline void row(const uint8_t* source, uint8_t* dest, int width, const uint8_t* lut)
{
	for (int x = 0; x < width; ++x, source += 3 * Step, dest += 3)
		pixel<ToneMapping, Limited>(source, dest, lut);
}
}
