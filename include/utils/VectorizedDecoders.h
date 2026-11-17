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

	// The first n bytes at p (n <= sizeof(T)), zero-extended.
	template<typename T>
	static inline T loadPartial(const uint8_t* p, size_t n)
	{
		T v = 0;
		std::memcpy(&v, p, std::min(n, sizeof(T)));
		return v;
	}

	static inline void putLut(uint8_t*& dest, const uint8_t* lutBuffer, uint32_t index)
	{
		std::memcpy(dest, lutBuffer + static_cast<size_t>(index) * 3, 3);
		dest += 3;
	}

	// Pixels left in the row, at most `group`.
	static inline size_t remaining(const uint8_t* dest, const uint8_t* destEnd, size_t group)
	{
		return std::min(static_cast<size_t>(destEnd - dest) / 3, group);
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
				// B G R (X): read the 3 colour bytes only.
				const uint32_t rgb = VECTOR_DETAIL::loadPartial<uint32_t>(currentSourceRGB, 3);
				const uint32_t b = rgb & 0xFF;
				const uint32_t g = (rgb >> 8) & 0xFF;
				const uint32_t r = (rgb >> 16) & 0xFF;
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
namespace VECTOR_PACKED_YUV {
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
		while (dest < destEnd) {
			const size_t pixels = remaining(dest, destEnd, group);
			// Whole macropixels only: one per pixel in quarter mode, one per two otherwise.
			const size_t macropixels = quarterMode ? pixels : (pixels + 1) / 2;
			const uint32_t m0 = load<uint32_t>(src);
			const uint32_t m1 = (macropixels > 1) ? load<uint32_t>(src + 4) : 0;
			src += 8;

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
				const uint32_t index[2] = { base0 | y00, base1 | y10 };
				for (size_t i = 0; i < pixels; ++i) putLut(dest, lutBuffer, index[i]);
			}
			else
			{
				const uint32_t index[4] = { base0 | y00, base0 | y01, base1 | y10, base1 | y11 };
				for (size_t i = 0; i < pixels; ++i) putLut(dest, lutBuffer, index[i]);
			}
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
// 4 pixels, or 2 (Y samples 0 and 2) in quarter mode. Bytes a partial step
// needs for its first `pixels` pixels, in samples:
//   Y:      pixels (full), 2*pixels - 1 (quarter)
//   chroma: (pixels + 1) / 2 pairs (full), pixels pairs (quarter)
namespace VECTOR_I420 {

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
		while (dest < destEnd) {
			const size_t pixels = remaining(dest, destEnd, group);
			const size_t ySamples = quarterMode ? 2 * pixels - 1 : pixels;
			const size_t pairs = quarterMode ? pixels : (pixels + 1) / 2;
			const uint32_t y = (pixels == group) ? load<uint32_t>(srcY) : loadPartial<uint32_t>(srcY, ySamples);
			const uint16_t u = (pixels == group) ? load<uint16_t>(srcU) : loadPartial<uint16_t>(srcU, pairs);
			const uint16_t v = (pixels == group) ? load<uint16_t>(srcV) : loadPartial<uint16_t>(srcV, pairs);
			srcY += 4; srcU += 2; srcV += 2;

			const uint32_t u0 = u & 0xFF;
			const uint32_t v0 = v & 0xFF;
			const uint32_t y00 = y & 0xFF;
			const uint32_t y01 = (y >> 8) & 0xFF;
			const uint32_t base0 = (u0 << 8) | (v0 << 16);
			
			const uint32_t u1 = (u >> 8) & 0xFF;
			const uint32_t v1 = (v >> 8) & 0xFF;
			const uint32_t y10 = (y >> 16) & 0xFF;
			const uint32_t y11 = (y >> 24) ;
			const uint32_t base1 = (u1 << 8) | (v1 << 16);
			
			if constexpr (quarterMode)
			{
				const uint32_t index[2] = { base0 | y00, base1 | y10 };
				for (size_t i = 0; i < pixels; ++i) putLut(dest, lutBuffer, index[i]);
			}
			else
			{
				const uint32_t index[4] = { base0 | y00, base0 | y01, base1 | y10, base1 | y11 };
				for (size_t i = 0; i < pixels; ++i) putLut(dest, lutBuffer, index[i]);
			}
		}
	}	
}


namespace VECTOR_NV12 {

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
		while (dest < destEnd) {
			const size_t pixels = remaining(dest, destEnd, group);
			const size_t ySamples = quarterMode ? 2 * pixels - 1 : pixels;
			const size_t pairs = quarterMode ? pixels : (pixels + 1) / 2;
			const uint32_t y = (pixels == group) ? load<uint32_t>(srcY) : loadPartial<uint32_t>(srcY, ySamples);
			const uint32_t uv = (pixels == group) ? load<uint32_t>(srcUV) : loadPartial<uint32_t>(srcUV, pairs * 2);
			srcY += 4; srcUV += 4;

			const uint32_t y00 = (y & 0xFF);
			const uint32_t y01 = (y >> 8) & 0xFF;
			const uint32_t base0 = (uv & 0x00'00'FF'FF) << 8;

			const uint32_t y10 = (y >> 16) & 0xFF;
			const uint32_t y11 = (y >> 24);
			const uint32_t base1 = (uv & 0xFF'FF'00'00) >> 8;

			if constexpr (quarterMode)
			{
				const uint32_t index[2] = { base0 | y00, base1 | y10 };
				for (size_t i = 0; i < pixels; ++i) putLut(dest, lutBuffer, index[i]);
			}
			else
			{
				const uint32_t index[4] = { base0 | y00, base0 | y01, base1 | y10, base1 | y11 };
				for (size_t i = 0; i < pixels; ++i) putLut(dest, lutBuffer, index[i]);
			}
		}	
	}
}

namespace VECTOR_P010 {

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
		while (dest < destEnd) {
			const size_t pixels = remaining(dest, destEnd, group);
			// 16-bit samples: Y and UV byte counts as for NV12, doubled.
			const size_t yBytes = (pixels == group) ? 8 : 2 * (quarterMode ? 2 * pixels - 1 : pixels);
			const size_t uvBytes = (pixels == group) ? 8 : 4 * (quarterMode ? pixels : (pixels + 1) / 2);
			const uint32_t y0 = loadPartial<uint32_t>(srcY, yBytes);
			const uint32_t y1 = yBytes > 4 ? loadPartial<uint32_t>(srcY + 4, yBytes - 4) : 0;
			const uint32_t uv0 = loadPartial<uint32_t>(srcUV, uvBytes);
			const uint32_t uv1 = uvBytes > 4 ? loadPartial<uint32_t>(srcUV + 4, uvBytes - 4) : 0;
			srcY += 8; srcUV += 8;

			const uint32_t u0 = (uv0 >> 8) & 0xFF;
			const uint32_t v0 = (uv0 >> 24);
			const uint32_t y00 = (y0 >> 8) & 0xFF;
			const uint32_t y01 = (y0 >> 24);
			const uint32_t base0 = (u0 << 8) | (v0 << 16);

			const uint32_t u1 = (uv1 >> 8) & 0xFF;
			const uint32_t v1 = (uv1 >> 24);
			const uint32_t y10 = (y1 >> 8) & 0xFF;
			const uint32_t y11 = (y1 >> 24);
			const uint32_t base1 = (u1 << 8) | (v1 << 16);

			if constexpr (quarterMode)
			{
				const uint32_t index[2] = { base0 | y00, base1 | y10 };
				for (size_t i = 0; i < pixels; ++i) putLut(dest, lutBuffer, index[i]);
			}
			else
			{
				const uint32_t index[4] = { base0 | y00, base0 | y01, base1 | y10, base1 | y11 };
				for (size_t i = 0; i < pixels; ++i) putLut(dest, lutBuffer, index[i]);
			}
		}
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
		while (dest < destEnd)
		{
			const size_t pixels = remaining(dest, destEnd, group);
			const size_t yBytes = (pixels == group) ? 8 : 2 * (quarterMode ? 2 * pixels - 1 : pixels);
			const size_t uvBytes = (pixels == group) ? 8 : 4 * (quarterMode ? pixels : (pixels + 1) / 2);
			const uint64_t y = loadPartial<uint64_t>(srcY, yBytes);
			const uint64_t uv = loadPartial<uint64_t>(srcUV, uvBytes);
			srcY += 8; srcUV += 8;

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

			if constexpr (quarterMode)
			{
				const uint32_t index[2] = { base0 | y00, base1 | y10 };
				for (size_t i = 0; i < pixels; ++i) putLut(dest, lutBuffer, index[i]);
			}
			else
			{
				const uint32_t index[4] = { base0 | y00, base0 | y01, base1 | y10, base1 | y11 };
				for (size_t i = 0; i < pixels; ++i) putLut(dest, lutBuffer, index[i]);
			}
		}
	}
}


