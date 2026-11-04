#pragma once
#include <cstdint>

namespace Bgr24
{
inline uint8_t expandLimited(uint8_t value)
{
	if (value <= 16) return 0;
	if (value >= 235) return 255;
	return static_cast<uint8_t>(((value - 16) * 255 + 109) / 219);
}

// Exactly three bytes read/written: safe at the last pixel of an MMAP plane.
// Conversion, range expansion and optional LUT happen in the output pass.
template<bool ToneMapping>
inline void pixel(const uint8_t* source, uint8_t* dest, bool limited, const uint8_t* lut)
{
	const uint8_t r = limited ? expandLimited(source[2]) : source[2];
	const uint8_t g = limited ? expandLimited(source[1]) : source[1];
	const uint8_t b = limited ? expandLimited(source[0]) : source[0];
	if constexpr (ToneMapping)
	{
		const auto index = (uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16)) * 3;
		dest[0] = lut[index]; dest[1] = lut[index + 1]; dest[2] = lut[index + 2];
	}
	else { dest[0] = r; dest[1] = g; dest[2] = b; }
}
}
