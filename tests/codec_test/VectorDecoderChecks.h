#pragma once
// `DecoderTest --vector-decoders`: the real FrameDecoder::processImageVector
// (through dispatchProcessImageVector, as GrabberWorker and FlatBuffersServer
// call it) and the VECTOR_* row decoders in VectorizedDecoders.h, against the
// independent reference in VectorDecoderReference.h (issue #61).
//
// Three ways to catch an access outside a buffer, because each misses
// something on its own:
//  - ASan on exact-size std::vector sources and LUT. ASan checks an unaligned
//    4-byte access only in its first 8-byte granule, so a read that starts
//    just before the end can pass unseen.
//  - Guard pages (POSIX): source planes and the LUT end exactly at a
//    PROT_NONE page, so any read past the end faults, whatever its alignment.
//    The fault handler prints the case before exiting.
//  - Sentinels: the output image's 16 spare bytes, and 16 bytes after each
//    row given to a VECTOR_* decoder, must be unchanged. A write past a row
//    lands inside the allocation (the next row, or the spare bytes), where
//    neither ASan nor a guard page sees it.
#include <utils/FrameDecoder.h>
// FrameDecoderUtils (the P010 tables) comes from main.cpp: the header has no
// include guard, so it can only be included once per translation unit.
#include <utils/VectorizedDecoders.h>
#include "VectorDecoderReference.h"
#include <csignal>
#include <csetjmp>
#include <memory>
#include <cstdio>
#include <cstdlib>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#define VECTOR_TEST_GUARD_PAGES 1
#endif

namespace VectorChecks
{
// Read-only bytes that end exactly where a PROT_NONE page begins.
class GuardedBytes
{
public:
	explicit GuardedBytes(const uint8_t* src, size_t size) : _size(size)
	{
#ifdef VECTOR_TEST_GUARD_PAGES
		const size_t page = size_t(sysconf(_SC_PAGESIZE));
		_mapped = ((size + page - 1) / page + 1) * page;
		void* m = mmap(nullptr, _mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (m == MAP_FAILED) { std::perror("mmap"); std::exit(2); }
		_base = static_cast<uint8_t*>(m);
		uint8_t* guard = _base + _mapped - page;
		mprotect(guard, page, PROT_NONE);
		_data = guard - size;
#else
		_copy.resize(size); _data = _copy.data();
#endif
		if (size)
			std::memcpy(_data, src, size);
	}
	~GuardedBytes()
	{
#ifdef VECTOR_TEST_GUARD_PAGES
		munmap(_base, _mapped);
#endif
	}
	GuardedBytes(const GuardedBytes&) = delete;
	GuardedBytes& operator=(const GuardedBytes&) = delete;
	uint8_t* data() const { return _data; }
private:
	size_t _size, _mapped = 0;
	uint8_t* _base = nullptr;
	uint8_t* _data = nullptr;
	std::vector<uint8_t> _copy;
};

inline const char* g_current = "";
#ifdef VECTOR_TEST_GUARD_PAGES
// A read past a guard page jumps back to the case that made it, which is
// reported as failed; the remaining cases still run.
inline sigjmp_buf g_jump;
inline volatile sig_atomic_t g_armed = 0;
inline void onFault(int sig)
{
	if (g_armed)
		siglongjmp(g_jump, sig);
	_exit(3);
}
// Runs fn(); returns its result, or a description of the fault it caused.
template<typename F>
std::string guardedRun(F&& fn)
{
	struct sigaction sa {}, oldSegv {}, oldBus {};
	sa.sa_handler = onFault;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NODEFER;
	sigaction(SIGSEGV, &sa, &oldSegv);
	sigaction(SIGBUS, &sa, &oldBus);
	std::string result;
	if (const int sig = sigsetjmp(g_jump, 1); sig == 0)
	{
		g_armed = 1;
		result = fn();
	}
	else
		result = std::string(sig == SIGSEGV ? "SIGSEGV" : "SIGBUS") + ": read past the end of a guarded source or LUT";
	g_armed = 0;
	sigaction(SIGSEGV, &oldSegv, nullptr);
	sigaction(SIGBUS, &oldBus, nullptr);
	return result;
}
#else
template<typename F>
std::string guardedRun(F&& fn) { return fn(); }
#endif

inline PixelFormat pixelFormat(VectorRef::Fmt f)
{
	switch (f)
	{
		case VectorRef::Fmt::YUYV: return PixelFormat::YUYV;
		case VectorRef::Fmt::UYVY: return PixelFormat::UYVY;
		case VectorRef::Fmt::NV12: return PixelFormat::NV12;
		case VectorRef::Fmt::I420: return PixelFormat::I420;
		case VectorRef::Fmt::P010: return PixelFormat::P010;
		case VectorRef::Fmt::RGB24: return PixelFormat::RGB24;
		default: return PixelFormat::XRGB;
	}
}

constexpr uint8_t kSentinel = 0xA5;
constexpr size_t kSpare = 16;   // Image allocates width*height*3 + 16 (ImageData::getMemory)

// One processImageVector call. Returns a failure description, or "" if it matches.
inline std::string runFull(const VectorRef::Case& c, const uint8_t* lut, bool guarded, const VectorRef::P010Tables& p010)
{
	const VectorRef::Frame fr = VectorRef::makeFrame(c.fmt, c.width, c.height, c.pad, uint32_t(c.width * 131 + c.height * 17 + c.pad));
	const VectorRef::Geometry g = VectorRef::geometry(fr, c.opts);
	const auto want = VectorRef::expected(fr, c.opts, p010);

	std::unique_ptr<GuardedBytes> gsrc;
	const uint8_t* src = fr.data.data();
	if (guarded) { gsrc = std::make_unique<GuardedBytes>(fr.data.data(), fr.data.size()); src = gsrc->data(); }

	Image<ColorRgb> image(unsigned(g.outW), unsigned(g.outH));   // pre-sized: resize() keeps this buffer
	const size_t bytes = size_t(g.outW) * g.outH * 3;
	std::memset(image.rawMem(), kSentinel, bytes + kSpare);
	const bool needsLut = !VectorRef::isRgb(c.fmt) || c.opts.tone;
	FrameDecoder::dispatchProcessImageVector[c.opts.quarter][c.opts.tone][false](
		c.opts.cropLeft, c.opts.cropRight, c.opts.cropTop, c.opts.cropBottom, src, nullptr, c.width, c.height, fr.line,
		pixelFormat(c.fmt), needsLut ? lut : nullptr, image, nullptr);

	if (int(image.width()) != g.outW || int(image.height()) != g.outH)
		return "output " + std::to_string(image.width()) + "x" + std::to_string(image.height()) + ", expected " + std::to_string(g.outW) + "x" + std::to_string(g.outH);
	const uint8_t* got = image.rawMem();
	for (size_t i = 0; i < bytes; ++i)
		if (got[i] != want[i])
		{
			char s[160];
			std::snprintf(s, sizeof(s), "pixel (%zu,%zu) channel %zu is %d, expected %d", (i / 3) % size_t(g.outW), (i / 3) / size_t(g.outW), i % 3, got[i], want[i]);
			return s;
		}
	for (size_t i = 0; i < kSpare; ++i)
		if (got[bytes + i] != kSentinel)
			return "wrote past the last row: spare byte " + std::to_string(i) + " changed";
	return "";
}

// One VECTOR_* call for output row 0 with every source plane row in its own
// exact, guarded buffer and 16 sentinel bytes after the destination row.
inline std::string runRow(VectorRef::Fmt f, int width, bool quarter, bool tone, const uint8_t* lut, const VectorRef::P010Tables& p010)
{
	using namespace VectorRef;
	const Frame fr = makeFrame(f, width, 2, 0, uint32_t(width * 7 + quarter));
	const Opts o{ quarter, tone, 0, 0, 0, 0 };
	const Geometry g = geometry(fr, o);
	if (g.outW <= 0)
		return "";
	std::vector<uint8_t> want = expected(fr, o, p010);
	want.resize(size_t(g.outW) * 3);   // row 0

	// Output row 0 comes from source row 0, or the bottom row for bottom-up RGB.
	const size_t rowBytes = size_t(width) * bytesPerPixel(f);
	GuardedBytes plane0(fr.data.data() + (isRgb(f) ? size_t(fr.line) * (fr.height - 1) : 0), rowBytes);
	std::unique_ptr<GuardedBytes> plane1, plane2;
	if (f == Fmt::NV12 || f == Fmt::P010)
		plane1 = std::make_unique<GuardedBytes>(fr.data.data() + fr.uvOffset(), rowBytes);
	if (f == Fmt::I420)
	{
		plane1 = std::make_unique<GuardedBytes>(fr.data.data() + fr.uvOffset(), size_t(width / 2));
		plane2 = std::make_unique<GuardedBytes>(fr.data.data() + fr.vOffset(), size_t(width / 2));
	}

	std::vector<uint8_t> dest(want.size() + kSpare, kSentinel);
	uint8_t* d = dest.data();
	const uint8_t* end = d + want.size();
	auto* p0 = plane0.data();
	const auto& ft = FrameDecoderUtils::instance();
	switch (f)
	{
		case Fmt::YUYV:
			quarter ? VECTOR_YUYV::process<true>(reinterpret_cast<uint32_t*>(p0), lut, d, end) : VECTOR_YUYV::process<false>(reinterpret_cast<uint32_t*>(p0), lut, d, end); break;
		case Fmt::UYVY:
			quarter ? VECTOR_UYVY::process<true>(reinterpret_cast<uint32_t*>(p0), lut, d, end) : VECTOR_UYVY::process<false>(reinterpret_cast<uint32_t*>(p0), lut, d, end); break;
		case Fmt::NV12:
			quarter ? VECTOR_NV12::process<true>(reinterpret_cast<uint32_t*>(p0), reinterpret_cast<uint32_t*>(plane1->data()), lut, d, end)
				: VECTOR_NV12::process<false>(reinterpret_cast<uint32_t*>(p0), reinterpret_cast<uint32_t*>(plane1->data()), lut, d, end); break;
		case Fmt::I420:
			quarter ? VECTOR_I420::process<true>(reinterpret_cast<uint32_t*>(p0), reinterpret_cast<uint16_t*>(plane1->data()), reinterpret_cast<uint16_t*>(plane2->data()), lut, d, end)
				: VECTOR_I420::process<false>(reinterpret_cast<uint32_t*>(p0), reinterpret_cast<uint16_t*>(plane1->data()), reinterpret_cast<uint16_t*>(plane2->data()), lut, d, end); break;
		case Fmt::P010:
			if (tone)
				quarter ? VECTOR_P010::processlWithToneMapping<true>(reinterpret_cast<uint64_t*>(p0), reinterpret_cast<uint64_t*>(plane1->data()), lut, d, end, ft.getLutP010_y(), ft.getlutP010_uv())
					: VECTOR_P010::processlWithToneMapping<false>(reinterpret_cast<uint64_t*>(p0), reinterpret_cast<uint64_t*>(plane1->data()), lut, d, end, ft.getLutP010_y(), ft.getlutP010_uv());
			else
				quarter ? VECTOR_P010::process<true>(reinterpret_cast<uint32_t*>(p0), reinterpret_cast<uint32_t*>(plane1->data()), lut, d, end)
					: VECTOR_P010::process<false>(reinterpret_cast<uint32_t*>(p0), reinterpret_cast<uint32_t*>(plane1->data()), lut, d, end);
			break;
		case Fmt::RGB24:
			if (tone) quarter ? VECTOR_RGB::decode<true, true, true>(p0, lut, d, end) : VECTOR_RGB::decode<true, false, true>(p0, lut, d, end);
			else quarter ? VECTOR_RGB::decode<true, true, false>(p0, lut, d, end) : VECTOR_RGB::decode<true, false, false>(p0, lut, d, end);
			break;
		case Fmt::XRGB:
			if (tone) quarter ? VECTOR_RGB::decode<false, true, true>(p0, lut, d, end) : VECTOR_RGB::decode<false, false, true>(p0, lut, d, end);
			else quarter ? VECTOR_RGB::decode<false, true, false>(p0, lut, d, end) : VECTOR_RGB::decode<false, false, false>(p0, lut, d, end);
			break;
	}
	for (size_t i = 0; i < want.size(); ++i)
		if (dest[i] != want[i])
			return "pixel " + std::to_string(i / 3) + " channel " + std::to_string(i % 3) + " is " + std::to_string(dest[i]) + ", expected " + std::to_string(want[i]);
	for (size_t i = 0; i < kSpare; ++i)
		if (dest[want.size() + i] != kSentinel)
			return "wrote past the row: guard byte " + std::to_string(i) + " changed";
	return "";
}

// Cases a separate, confirmed defect still breaks; printed, never silent.
inline const char* knownDefect(const VectorRef::Case& c)
{
	if (c.fmt == VectorRef::Fmt::P010 && !c.opts.quarter && (c.opts.cropLeft >> 1) > 0)
		return "P010 left crop is applied in bytes, not pixels (#63)";
	return nullptr;
}
}

inline int checkVectorDecoders()
{
	using namespace VectorChecks;
	const auto lutVec = VectorRef::buildLut();
	const auto& ft = FrameDecoderUtils::instance();
	const VectorRef::P010Tables p010{ &ft.getLutP010_y(), &ft.getlutP010_uv() };
	GuardedBytes guardedLut(lutVec.data(), lutVec.size());

	int failed = 0, passed = 0, skipped = 0;
	std::string label;

	// Row decoders, each row alone with guards on every side.
	const VectorRef::Fmt fmts[] = { VectorRef::Fmt::YUYV, VectorRef::Fmt::UYVY, VectorRef::Fmt::NV12, VectorRef::Fmt::I420, VectorRef::Fmt::P010, VectorRef::Fmt::RGB24, VectorRef::Fmt::XRGB };
	for (auto f : fmts)
		for (bool quarter : { false, true })
			for (bool tone : { false, true })
			{
				if (tone && f != VectorRef::Fmt::P010 && !VectorRef::isRgb(f))
					continue;   // other YUV row decoders have no tone-mapping variant
				for (int w = VectorRef::isRgb(f) ? 1 : 2; w <= 14; w += VectorRef::isRgb(f) ? 1 : 2)
				{
					label = std::string("row ") + VectorRef::name(f) + " width " + std::to_string(w) + (quarter ? " quarter" : "") + (tone ? " tone-mapped" : "");
					g_current = label.c_str();
					const std::string err = guardedRun([&] { return runRow(f, w, quarter, tone, guardedLut.data(), p010); });
					if (err.empty()) ++passed;
					else { ++failed; std::printf("FAIL %s: %s\n", label.c_str(), err.c_str()); }
				}
			}
	std::printf("%s row decoders: %d passed, %d failed\n", failed ? "FAIL" : "PASS", passed, failed);

	// Whole frames through the dispatch table: ASan-checked vectors, then guard pages.
	const int rowFailed = failed;
	for (bool guarded : { false, true })
	{
		int p = 0, f = 0;
		for (const auto& c : VectorRef::cases())
		{
			label = VectorRef::label(c) + (guarded ? " [guard pages]" : "");
			if (const char* why = knownDefect(c))
			{
				if (!guarded) { ++skipped; std::printf("SKIP %s: %s\n", label.c_str(), why); }
				continue;
			}
			g_current = label.c_str();
			const std::string err = guardedRun([&] { return runFull(c, guarded ? guardedLut.data() : lutVec.data(), guarded, p010); });
			if (err.empty()) ++p;
			else { ++f; std::printf("FAIL %s: %s\n", label.c_str(), err.c_str()); }
		}
		std::printf("%s processImageVector%s: %d cases passed, %d failed\n", f ? "FAIL" : "PASS", guarded ? " (guard pages)" : "", p, f);
		passed += p; failed += f;
	}
	(void)rowFailed;
	if (failed) { std::printf("FAIL vector decoders: %d failing, %d passing, %d skipped\n", failed, passed, skipped); return 1; }
	std::printf("PASS vector decoders: %d checks, %d skipped\n", passed, skipped);
	return 0;
}
