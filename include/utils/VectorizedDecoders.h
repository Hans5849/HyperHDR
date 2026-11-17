/* VectorizedDecoders.h
*
*  MIT License
*
*  Copyright (c) 2020-2026 awawa-dev
*
*  Project homesite: https://github.com/awawa-dev/HyperHDR
*
*  Permission is hereby granted, free of charge, to any person obtaining a copy
*  of this software and associated documentation files (the "Software"), to deal
*  in the Software without restriction, including without limitation the rights
*  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
*  copies of the Software, and to permit persons to whom the Software is
*  furnished to do so, subject to the following conditions:
*
*  The above copyright notice and this permission notice shall be included in all
*  copies or substantial portions of the Software.

*  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
*  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
*  SOFTWARE.
*/

#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <array>
#include <algorithm>

// Every access below is bounded and alignment-free (#61):
//  - source words are read with memcpy, so a row may start at any address
//    (cropped rows, odd strides);
//  - a LUT entry is 3 bytes: exactly 3 are read and 3 written per pixel;
//  - the decoders emit several pixels per step. A row's last step may be
//    partial (output width not a multiple of the step): it reads only the
//    source bytes those pixels use and writes only those pixels.
// Values, channel order and LUT indexing are as before. Little-endian
// byte order is assumed, as before.
namespace VECTOR_DETAIL {
	template<typename T>
	static inline T load(const uint8_t* p)
	{
		T v;
		std::memcpy(&v, p, sizeof(T));
		return v;
	}

	// The first n bytes at p (n < sizeof(T)), zero-extended. Only for a row's
	// last, partial step, so it's off the hot path.
	template<typename T>
	static inline T loadPartial(const uint8_t* p, size_t n)
	{
		T v = 0;
		for (size_t i = 0; i < n && i < sizeof(T); ++i)
			v |= static_cast<T>(p[i]) << (8 * i);
		return v;
	}

	static inline void putLut(uint8_t*& dest, const uint8_t* lutBuffer, uint32_t index)
	{
		std::memcpy(dest, lutBuffer + static_cast<size_t>(index) * 3, 3);
		dest += 3;
	}

	// LUT entry as a 32-bit word whose low 3 bytes are the pixel. For every
	// index but the last, the 4th byte read is the next entry's first, inside
	// even an exact 256^3*3 LUT; the last entry (white) is read with 3 bytes.
	static inline uint32_t lutWord(const uint8_t* lutBuffer, uint32_t index)
	{
		const uint8_t* e = lutBuffer + static_cast<size_t>(index) * 3;
		uint32_t v;
		if (index < 0xFFFFFFu) [[likely]]
			std::memcpy(&v, e, 4);
		else
			v = uint32_t(e[0]) | (uint32_t(e[1]) << 8) | (uint32_t(e[2]) << 16);
		return v;
	}

	// A pixel that isn't the last of its step: 4 bytes stored, the 4th is
	// overwritten by the next pixel, inside the row.
	static inline void putLutInner(uint8_t*& dest, const uint8_t* lutBuffer, uint32_t index)
	{
		const uint32_t v = lutWord(lutBuffer, index);
		std::memcpy(dest, &v, 4);
		dest += 3;
	}

	// The last pixel of a step: exactly 3 bytes stored.
	static inline void putLutLast(uint8_t*& dest, const uint8_t* lutBuffer, uint32_t index)
	{
		const uint32_t v = lutWord(lutBuffer, index);
		std::memcpy(dest, &v, 3);
		dest += 3;
	}

	// A whole step: 2 or 4 pixels, unrolled.
	template<size_t N>
	static inline void putStep(uint8_t*& dest, const uint8_t* lutBuffer, const uint32_t (&index)[N])
	{
		putLutInner(dest, lutBuffer, index[0]);
		if constexpr (N == 4)
		{
			putLutInner(dest, lutBuffer, index[1]);
			putLutInner(dest, lutBuffer, index[2]);
			putLutLast(dest, lutBuffer, index[3]);
		}
		else
			putLutLast(dest, lutBuffer, index[1]);
	}

	// Emits the first `count` of a step's pixels (count < group, the row's end).
	template<size_t N>
	static inline void putPartial(uint8_t*& dest, const uint8_t* lutBuffer, const uint32_t (&index)[N], size_t count)
	{
		for (size_t i = 0; i < count && i < N; ++i)
			putLut(dest, lutBuffer, index[i]);
	}

	// Pixels left in the row.
	static inline size_t remaining(const uint8_t* dest, const uint8_t* destEnd)
	{
		return static_cast<size_t>(destEnd - dest) / 3;
	}
}

namespace VECTOR_RGB {
	template<bool isByte3Input, bool quarterMode, bool toneMapping>
	static inline void decode(
		uint8_t* currentSourceRGB,
		const uint8_t* lutBuffer,
		uint8_t* dest,
		const uint8_t* destEnd
	)
	{
		constexpr int stride = (isByte3Input ? 3 : 4) * (quarterMode ? 2 : 1);
		while (dest < destEnd) {
			if constexpr (toneMapping)
			{
				// B G R (X): the 3 colour bytes only.
				const uint32_t b = currentSourceRGB[0];
				const uint32_t g = currentSourceRGB[1];
				const uint32_t r = currentSourceRGB[2];
				VECTOR_DETAIL::putLut(dest, lutBuffer, r | (g << 8) | (b << 16));
			}
			else
			{
				*(dest++) = currentSourceRGB[2];
				*(dest++) = currentSourceRGB[1];
				*(dest++) = currentSourceRGB[0];
			}
			currentSourceRGB += stride;
		}
	}
}

// YUYV and UYVY: one 4-byte macropixel holds 2 pixels. A step reads 2
// macropixels and emits 4 pixels, or 2 (the first of each) in quarter mode.
// A row's last step may be partial: it reads whole macropixels only (one per
// pixel in quarter mode, one per two otherwise).
namespace VECTOR_PACKED_YUV {
	template<bool quarterMode, bool uyvy>
	static inline void indices(uint32_t m0, uint32_t m1, uint32_t (&index)[quarterMode ? 2 : 4])
	{
		const uint32_t u0 = uyvy ? (m0 & 0xFF) : ((m0 >> 8) & 0xFF);
		const uint32_t v0 = uyvy ? ((m0 >> 16) & 0xFF) : (m0 >> 24);
		const uint32_t y00 = uyvy ? ((m0 >> 8) & 0xFF) : (m0 & 0xFF);
		const uint32_t y01 = uyvy ? (m0 >> 24) : ((m0 >> 16) & 0xFF);
		const uint32_t base0 = (u0 << 8) | (v0 << 16);

		const uint32_t u1 = uyvy ? (m1 & 0xFF) : ((m1 >> 8) & 0xFF);
		const uint32_t v1 = uyvy ? ((m1 >> 16) & 0xFF) : (m1 >> 24);
		const uint32_t y10 = uyvy ? ((m1 >> 8) & 0xFF) : (m1 & 0xFF);
		const uint32_t y11 = uyvy ? (m1 >> 24) : ((m1 >> 16) & 0xFF);
		const uint32_t base1 = (u1 << 8) | (v1 << 16);

		if constexpr (quarterMode)
		{
			index[0] = base0 | y00; index[1] = base1 | y10;
		}
		else
		{
			index[0] = base0 | y00; index[1] = base0 | y01; index[2] = base1 | y10; index[3] = base1 | y11;
		}
	}

	template<bool quarterMode, bool uyvy>
	static inline void process(
		uint32_t* currentSourceYUV,
		const uint8_t* lutBuffer,
		uint8_t* dest,
		const uint8_t* destEnd
	)
	{
		using namespace VECTOR_DETAIL;
		constexpr size_t group = quarterMode ? 2 : 4;
		const uint8_t* src = reinterpret_cast<const uint8_t*>(currentSourceYUV);
		uint32_t index[group];
		while (remaining(dest, destEnd) >= group) {
			indices<quarterMode, uyvy>(load<uint32_t>(src), load<uint32_t>(src + 4), index);
			src += 8;
			putStep(dest, lutBuffer, index);
		}
		if (const size_t pixels = remaining(dest, destEnd)) {
			const size_t macropixels = quarterMode ? pixels : (pixels + 1) / 2;
			indices<quarterMode, uyvy>(load<uint32_t>(src), macropixels > 1 ? load<uint32_t>(src + 4) : 0, index);
			putPartial(dest, lutBuffer, index, pixels);
		}
	}
}

namespace VECTOR_UYVY {

	template<bool quarterMode>
	static inline void process(
		uint32_t* currentSourceYUV,
		const uint8_t* lutBuffer,
		uint8_t* dest,
		const uint8_t* destEnd
	)
	{
		VECTOR_PACKED_YUV::process<quarterMode, true>(currentSourceYUV, lutBuffer, dest, destEnd);
	}

}

namespace VECTOR_YUYV{

	template<bool quarterMode>
	static inline void process(
		uint32_t* currentSourceYUV,
		const uint8_t* lutBuffer,
		uint8_t* dest,
		const uint8_t* destEnd
	)
	{
		VECTOR_PACKED_YUV::process<quarterMode, false>(currentSourceYUV, lutBuffer, dest, destEnd);
	}
}

// Planar: a step reads 4 Y samples and the chroma of 2 pixel pairs, and emits
// 4 pixels, or 2 (Y samples 0 and 2) in quarter mode. A row's last, partial
// step reads only the samples its pixels use:
//   Y samples:    pixels (full), 2*pixels - 1 (quarter)
//   chroma pairs: (pixels + 1) / 2 (full), pixels (quarter)
namespace VECTOR_PLANAR {
	template<bool quarterMode> constexpr size_t ySamples(size_t pixels) { return quarterMode ? 2 * pixels - 1 : pixels; }
	template<bool quarterMode> constexpr size_t chromaPairs(size_t pixels) { return quarterMode ? pixels : (pixels + 1) / 2; }

	template<bool quarterMode>
	static inline void indices(uint32_t y00, uint32_t y01, uint32_t y10, uint32_t y11, uint32_t base0, uint32_t base1,
		uint32_t (&index)[quarterMode ? 2 : 4])
	{
		if constexpr (quarterMode)
		{
			index[0] = base0 | y00; index[1] = base1 | y10;
		}
		else
		{
			index[0] = base0 | y00; index[1] = base0 | y01; index[2] = base1 | y10; index[3] = base1 | y11;
		}
	}
}

namespace VECTOR_I420 {

	template<bool quarterMode>
	static inline void indices(uint32_t y, uint32_t u, uint32_t v, uint32_t (&index)[quarterMode ? 2 : 4])
	{
		const uint32_t base0 = ((u & 0xFF) << 8) | ((v & 0xFF) << 16);
		const uint32_t base1 = (((u >> 8) & 0xFF) << 8) | (((v >> 8) & 0xFF) << 16);
		VECTOR_PLANAR::indices<quarterMode>(y & 0xFF, (y >> 8) & 0xFF, (y >> 16) & 0xFF, y >> 24, base0, base1, index);
	}

	template<bool quarterMode>
	static inline void process(
		uint32_t* currentSourceY,
		uint16_t* currentSourceU,
		uint16_t* currentSourceV,
		const uint8_t* lutBuffer,
		uint8_t* dest,
		const uint8_t* destEnd
	)
	{
		using namespace VECTOR_DETAIL;
		constexpr size_t group = quarterMode ? 2 : 4;
		const uint8_t* srcY = reinterpret_cast<const uint8_t*>(currentSourceY);
		const uint8_t* srcU = reinterpret_cast<const uint8_t*>(currentSourceU);
		const uint8_t* srcV = reinterpret_cast<const uint8_t*>(currentSourceV);
		uint32_t index[group];
		while (remaining(dest, destEnd) >= group) {
			indices<quarterMode>(load<uint32_t>(srcY), load<uint16_t>(srcU), load<uint16_t>(srcV), index);
			srcY += 4; srcU += 2; srcV += 2;
			putStep(dest, lutBuffer, index);
		}
		if (const size_t pixels = remaining(dest, destEnd)) {
			const size_t pairs = VECTOR_PLANAR::chromaPairs<quarterMode>(pixels);
			indices<quarterMode>(loadPartial<uint32_t>(srcY, VECTOR_PLANAR::ySamples<quarterMode>(pixels)),
				loadPartial<uint16_t>(srcU, pairs), loadPartial<uint16_t>(srcV, pairs), index);
			putPartial(dest, lutBuffer, index, pixels);
		}
	}	
}


namespace VECTOR_NV12 {

	template<bool quarterMode>
	static inline void indices(uint32_t y, uint32_t uv, uint32_t (&index)[quarterMode ? 2 : 4])
	{
		const uint32_t base0 = (uv & 0x00'00'FF'FF) << 8;
		const uint32_t base1 = (uv & 0xFF'FF'00'00) >> 8;
		VECTOR_PLANAR::indices<quarterMode>(y & 0xFF, (y >> 8) & 0xFF, (y >> 16) & 0xFF, y >> 24, base0, base1, index);
	}

	template<bool quarterMode>
	static inline void process(
		uint32_t* currentSourceY,
		uint32_t* currentSourceUV,
		const uint8_t* lutBuffer,
		uint8_t* dest,
		const uint8_t* destEnd
	)
	{
		using namespace VECTOR_DETAIL;
		constexpr size_t group = quarterMode ? 2 : 4;
		const uint8_t* srcY = reinterpret_cast<const uint8_t*>(currentSourceY);
		const uint8_t* srcUV = reinterpret_cast<const uint8_t*>(currentSourceUV);
		uint32_t index[group];
		while (remaining(dest, destEnd) >= group) {
			indices<quarterMode>(load<uint32_t>(srcY), load<uint32_t>(srcUV), index);
			srcY += 4; srcUV += 4;
			putStep(dest, lutBuffer, index);
		}
		if (const size_t pixels = remaining(dest, destEnd)) {
			indices<quarterMode>(loadPartial<uint32_t>(srcY, VECTOR_PLANAR::ySamples<quarterMode>(pixels)),
				loadPartial<uint32_t>(srcUV, 2 * VECTOR_PLANAR::chromaPairs<quarterMode>(pixels)), index);
			putPartial(dest, lutBuffer, index, pixels);
		}
	}
}

namespace VECTOR_P010 {

	// 16-bit little-endian samples; the decoder uses each sample's high byte.
	template<bool quarterMode>
	static inline void indices(uint32_t y0, uint32_t y1, uint32_t uv0, uint32_t uv1, uint32_t (&index)[quarterMode ? 2 : 4])
	{
		const uint32_t base0 = (((uv0 >> 8) & 0xFF) << 8) | ((uv0 >> 24) << 16);
		const uint32_t base1 = (((uv1 >> 8) & 0xFF) << 8) | ((uv1 >> 24) << 16);
		VECTOR_PLANAR::indices<quarterMode>((y0 >> 8) & 0xFF, y0 >> 24, (y1 >> 8) & 0xFF, y1 >> 24, base0, base1, index);
	}

	template<bool quarterMode>
	static inline void process(
		uint32_t* currentSourceY,
		uint32_t* currentSourceUV,
		const uint8_t* lutBuffer,
		uint8_t* dest,
		const uint8_t* destEnd
	)
	{
		using namespace VECTOR_DETAIL;
		constexpr size_t group = quarterMode ? 2 : 4;
		const uint8_t* srcY = reinterpret_cast<const uint8_t*>(currentSourceY);
		const uint8_t* srcUV = reinterpret_cast<const uint8_t*>(currentSourceUV);
		uint32_t index[group];
		while (remaining(dest, destEnd) >= group) {
			indices<quarterMode>(load<uint32_t>(srcY), load<uint32_t>(srcY + 4), load<uint32_t>(srcUV), load<uint32_t>(srcUV + 4), index);
			srcY += 8; srcUV += 8;
			putStep(dest, lutBuffer, index);
		}
		if (const size_t pixels = remaining(dest, destEnd)) {
			const size_t yBytes = 2 * VECTOR_PLANAR::ySamples<quarterMode>(pixels);
			const size_t uvBytes = 4 * VECTOR_PLANAR::chromaPairs<quarterMode>(pixels);
			indices<quarterMode>(loadPartial<uint32_t>(srcY, yBytes), yBytes > 4 ? loadPartial<uint32_t>(srcY + 4, yBytes - 4) : 0,
				loadPartial<uint32_t>(srcUV, uvBytes), uvBytes > 4 ? loadPartial<uint32_t>(srcUV + 4, uvBytes - 4) : 0, index);
			putPartial(dest, lutBuffer, index, pixels);
		}
	}

	template<bool quarterMode>
	static inline void indicesToneMapped(uint64_t y, uint64_t uv, const std::array<uint8_t, 1024>& lutP010_y,
		const std::array<uint8_t, 1024>& lutP010_uv, uint32_t (&index)[quarterMode ? 2 : 4])
	{
		const uint32_t u0 = lutP010_uv[(uv & 0xFF'FF) >> 6];
		const uint32_t v0 = lutP010_uv[(uv & 0xFF'FF'00'00) >> (16 + 6)];
		const uint32_t y00 = lutP010_y[(y & 0xFF'FF) >> 6];
		const uint32_t y01 = lutP010_y[(y & 0xFF'FF'00'00) >> (16 + 6)];
		const uint32_t base0 = (u0 << 8) | (v0 << 16);

		const uint32_t u1 = lutP010_uv[(uv & 0xFF'FF'00'00'00'00) >> (6 + 32)];
		const uint32_t v1 = lutP010_uv[uv >> (16 + 6 + 32)];
		const uint32_t y10 = lutP010_y[(y & 0xFF'FF'00'00'00'00) >> (6 + 32)];
		const uint32_t y11 = lutP010_y[y >> (16 + 6 + 32)];
		const uint32_t base1 = (u1 << 8) | (v1 << 16);

		VECTOR_PLANAR::indices<quarterMode>(y00, y01, y10, y11, base0, base1, index);
	}
	
	template<bool quarterMode>
	static inline void processlWithToneMapping(
		uint64_t* currentSourceY,
		uint64_t* currentSourceUV,
		const uint8_t* lutBuffer,
		uint8_t* dest,
		const uint8_t* destEnd,
		const std::array<uint8_t, 1024>& lutP010_y,
		const std::array<uint8_t, 1024>& lutP010_uv
	)
	{
		using namespace VECTOR_DETAIL;
		constexpr size_t group = quarterMode ? 2 : 4;
		const uint8_t* srcY = reinterpret_cast<const uint8_t*>(currentSourceY);
		const uint8_t* srcUV = reinterpret_cast<const uint8_t*>(currentSourceUV);
		uint32_t index[group];
		while (remaining(dest, destEnd) >= group)
		{
			indicesToneMapped<quarterMode>(load<uint64_t>(srcY), load<uint64_t>(srcUV), lutP010_y, lutP010_uv, index);
			srcY += 8; srcUV += 8;
			putStep(dest, lutBuffer, index);
		}
		if (const size_t pixels = remaining(dest, destEnd))
		{
			indicesToneMapped<quarterMode>(loadPartial<uint64_t>(srcY, 2 * VECTOR_PLANAR::ySamples<quarterMode>(pixels)),
				loadPartial<uint64_t>(srcUV, 4 * VECTOR_PLANAR::chromaPairs<quarterMode>(pixels)), lutP010_y, lutP010_uv, index);
			putPartial(dest, lutBuffer, index, pixels);
		}
	}
}


