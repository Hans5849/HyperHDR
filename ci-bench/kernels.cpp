// Synthetic microbenchmark of the VECTOR_* row decoders (temporary, not for
// merge): one 1920x1080 frame of random bytes, decoded row by row; median of 11
// rounds of 10 frames, ms per frame. Built twice: -DHDR=<master header> and
// -DHDR=<#64 header>.
#include HDR
#include <utils/FrameDecoderUtils.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
int main()
{
	constexpr int W = 1920, H = 1080;
	std::vector<uint8_t> lut(256u * 256 * 256 * 3 + 64), src(size_t(W) * H * 4 + 64), dst(size_t(W) * H * 3 + 64);
	std::mt19937 g(7);
	for (auto& b : lut) b = uint8_t(g());
	for (auto& b : src) b = uint8_t(g());
	const auto& ft = FrameDecoderUtils::instance();
	auto time = [&](const char* name, auto&& frame) {
		std::vector<double> t;
		for (int r = 0; r < 11; ++r)
		{
			auto a = std::chrono::steady_clock::now();
			for (int i = 0; i < 10; ++i) frame();
			t.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count() / 10);
		}
		std::sort(t.begin(), t.end());
		std::printf("%s %.3f\n", name, t[5]);
	};
	uint8_t* s = src.data();
	uint8_t* d = dst.data();
	const uint8_t* L = lut.data();
#define ROWS(Q, body) for (int y = 0; y < (Q ? H / 2 : H); ++y) { uint8_t* dr = d + size_t(y) * (Q ? W / 2 : W) * 3; const uint8_t* de = dr + (Q ? W / 2 : W) * 3; const int sy = Q ? 2 * y : y; body; }
	for (bool q : { false, true })
	{
		const char* sfx = q ? "-quarter" : "";
		char n[64];
		auto nm = [&](const char* f) { std::snprintf(n, sizeof(n), "%s%s", f, sfx); return n; };
		if (q) {
			time(nm("YUYV"), [&] { ROWS(true, VECTOR_YUYV::process<true>((uint32_t*)(s + size_t(sy) * W * 2), L, dr, de)) });
			time(nm("UYVY"), [&] { ROWS(true, VECTOR_UYVY::process<true>((uint32_t*)(s + size_t(sy) * W * 2), L, dr, de)) });
			time(nm("NV12"), [&] { ROWS(true, VECTOR_NV12::process<true>((uint32_t*)(s + size_t(sy) * W), (uint32_t*)(s + size_t(W) * H + size_t(sy / 2) * W), L, dr, de)) });
			time(nm("I420"), [&] { ROWS(true, VECTOR_I420::process<true>((uint32_t*)(s + size_t(sy) * W), (uint16_t*)(s + size_t(W) * H + size_t(sy / 2) * W / 2), (uint16_t*)(s + size_t(W) * H * 5 / 4 + size_t(sy / 2) * W / 2), L, dr, de)) });
			time(nm("P010"), [&] { ROWS(true, VECTOR_P010::process<true>((uint32_t*)(s + size_t(sy) * W * 2), (uint32_t*)(s + size_t(W) * H * 2 + size_t(sy / 2) * W * 2), L, dr, de)) });
			time(nm("P010-tone"), [&] { ROWS(true, VECTOR_P010::processlWithToneMapping<true>((uint64_t*)(s + size_t(sy) * W * 2), (uint64_t*)(s + size_t(W) * H * 2 + size_t(sy / 2) * W * 2), L, dr, de, ft.getLutP010_y(), ft.getlutP010_uv())) });
			time(nm("RGB24-tone"), [&] { ROWS(true, (VECTOR_RGB::decode<true, true, true>(s + size_t(sy) * W * 3, L, dr, de))) });
		} else {
			time(nm("YUYV"), [&] { ROWS(false, VECTOR_YUYV::process<false>((uint32_t*)(s + size_t(sy) * W * 2), L, dr, de)) });
			time(nm("UYVY"), [&] { ROWS(false, VECTOR_UYVY::process<false>((uint32_t*)(s + size_t(sy) * W * 2), L, dr, de)) });
			time(nm("NV12"), [&] { ROWS(false, VECTOR_NV12::process<false>((uint32_t*)(s + size_t(sy) * W), (uint32_t*)(s + size_t(W) * H + size_t(sy / 2) * W), L, dr, de)) });
			time(nm("I420"), [&] { ROWS(false, VECTOR_I420::process<false>((uint32_t*)(s + size_t(sy) * W), (uint16_t*)(s + size_t(W) * H + size_t(sy / 2) * W / 2), (uint16_t*)(s + size_t(W) * H * 5 / 4 + size_t(sy / 2) * W / 2), L, dr, de)) });
			time(nm("P010"), [&] { ROWS(false, VECTOR_P010::process<false>((uint32_t*)(s + size_t(sy) * W * 2), (uint32_t*)(s + size_t(W) * H * 2 + size_t(sy / 2) * W * 2), L, dr, de)) });
			time(nm("P010-tone"), [&] { ROWS(false, VECTOR_P010::processlWithToneMapping<false>((uint64_t*)(s + size_t(sy) * W * 2), (uint64_t*)(s + size_t(W) * H * 2 + size_t(sy / 2) * W * 2), L, dr, de, ft.getLutP010_y(), ft.getlutP010_uv())) });
			time(nm("RGB24-tone"), [&] { ROWS(false, (VECTOR_RGB::decode<true, false, true>(s + size_t(sy) * W * 3, L, dr, de))) });
		}
	}
}
