#pragma once
#include <utils/FrameDecoder.h>
#include <utils/Bgr24.h>
#include "BenchReport.h"
#include "RawFrameInput.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

// Exercise the real dispatch table, not a second implementation of the decoder.
inline int checkBgr24Decoder()
{
	constexpr int width = 6, height = 4, stride = 20;
	// No padding after the final pixel: catches accidental four-byte reads.
	std::vector<uint8_t> source((height - 1) * stride + width * 3, 0xee);
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			source[y * stride + x * 3] = 16 + x * 20;
			source[y * stride + x * 3 + 1] = 16 + y * 60;
			source[y * stride + x * 3 + 2] = x == 0 ? 235 : 200 - x * 15;
		}
	std::vector<uint8_t> lut(256 * 256 * 256 * 3);
	for (bool limited : {false, true})
		for (bool quarter : {false, true})
			for (bool tone : {false, true})
				for (bool crop : {false, true})
				{
					const int left = crop ? 2 : 0, top = crop ? 1 : 0, bottom = crop ? 1 : 0;
					const int outWidth = quarter ? width / 2 : width - left;
					const int outHeight = quarter ? height / 2 : height - top - bottom;
					for (int y = 0; y < height; ++y)
						for (int x = 0; x < width; ++x)
						{
							uint8_t rgb[3]; Bgr24::pixel<false>(&source[y * stride + x * 3], rgb, limited, nullptr);
							const auto index = (uint32_t(rgb[0]) | (uint32_t(rgb[1]) << 8) | (uint32_t(rgb[2]) << 16)) * 3;
							lut[index] = rgb[0] ^ 0x55; lut[index + 1] = rgb[1] ^ 0xaa; lut[index + 2] = rgb[2] ^ 0xff;
						}
					Image<ColorRgb> image;
					FrameDecoder::dispatchProcessImageVector[quarter][tone][false](left, 0, top, bottom,
						source.data(), nullptr, width, height, stride,
						limited ? PixelFormat::BGR24_LIMITED : PixelFormat::BGR24, lut.data(), image, nullptr);
					if (image.width() != unsigned(outWidth) || image.height() != unsigned(outHeight)) return 1;
					for (int y = 0; y < outHeight; ++y)
						for (int x = 0; x < outWidth; ++x)
						{
							const int sx = quarter ? x * 2 : x + left, sy = quarter ? y * 2 : y + top;
							const auto* p = &source[sy * stride + sx * 3];
							auto expected = [&](int c) { return limited ? Bgr24::expandLimited(p[c]) : p[c]; };
							const auto& actual = image(x, y);
							if (actual.red != (expected(2) ^ (tone ? 0x55 : 0)) ||
								actual.green != (expected(1) ^ (tone ? 0xaa : 0)) ||
								actual.blue != (expected(0) ^ (tone ? 0xff : 0)))
							{
								std::cerr << "FAIL BGR24: orientation, stride, crop, range or LUT mismatch\n";
								return 1;
							}
						}
				}
	std::cout << "PASS 16 BGR24 decoder combinations (range, scale, LUT, crop, padded stride, top-down rows)\n";
	return 0;
}

// Time the real BGR24 dispatch at the verified mainline layout (1920x1080,
// stride 5760, one plane) for each range/scale/LUT case. Single-threaded, so
// pin it to one core to compare A76 and A55 (issue #12):
//   taskset -c 4 build-codec/bin/DecoderTest --bgr24-bench    # RK3588 A76
//   taskset -c 0 build-codec/bin/DecoderTest --bgr24-bench    # RK3588 A55
// Optional (see docs/rk3588-benchmark-reporting.md): --json FILE, --csv FILE,
// --raw, --pattern random,gradient,solid, --iterations N, --warmup N,
// --commit SHA, --label NAME. Without options the output is the original table.
inline int benchBgr24Decoder(const BenchReport::Options& options = {})
{
	if (!options.error.empty())
	{
		std::fprintf(stderr, "bgr24-bench: %s\n", options.error.c_str());
		return 2;
	}
	// Synthetic inputs use the verified mainline layout; --input uses the saved
	// frame's own layout, as its description states (RawFrameInput.h).
	std::optional<RawFrame::Frame> file;
	if (options.inputPath)
	{
		auto loaded = RawFrame::load(*options.inputPath, *options.inputDescription);
		if (!loaded.frame)
		{
			std::fprintf(stderr, "bgr24-bench: %s\n", loaded.error.c_str());
			return 2;
		}
		file = std::move(loaded.frame);
	}
	const int width = file ? file->description.width : 1920;
	const int height = file ? file->description.height : 1080;
	const int stride = file ? file->description.stride : 5760;
	constexpr uint32_t seed = 12345;
	std::vector<uint8_t> source = file ? file->bytes : std::vector<uint8_t>(size_t(height - 1) * stride + width * 3);
	std::vector<uint8_t> lut(size_t(256) * 256 * 256 * 3, 0x80);   // full-size LUT, as captured frames use

	BenchReport::Run run;
	run.width = width; run.height = height; run.stride = stride;
	std::vector<std::string> patterns = options.patterns;
	if (file)
	{
		const auto& d = file->description;
		run.input.kind = "file";
		run.input.name = file->name;
		run.input.sha256 = file->sha256;
		run.input.format = d.format;
		run.input.declaredRange = d.range;
		run.input.orientation = d.orientation;
		run.input.payloadBytes = int64_t(d.payload);
		run.input.source = d.source;
		patterns = { RawFrame::patternName(*file) };
	}
	run.lutBytes = lut.size(); run.lutFill = "constant 0x80"; run.seed = seed;
	run.meta = BenchReport::collect(BenchReport::SysRoot{}, BenchReport::processAffinity(), BenchReport::currentCpu());
	run.meta.commit = options.commit;
	run.meta.label = options.label;

	for (const auto& pattern : patterns)
	{
		if (!file)
		{
			std::fill(source.begin(), source.end(), uint8_t(0));
			BenchReport::fillPattern(pattern, source, width, height, stride, seed);
		}
		for (bool limited : {false, true})
			for (bool quarter : {false, true})
				for (bool tone : {false, true})
				{
					BenchReport::Case c;
					c.range = limited ? "limited" : "full";
					c.output = quarter ? "quarter" : "full";
					c.lut = tone;
					c.pattern = pattern;
					c.warmup = options.warmup;
					if (tone)
						c.profile = BenchReport::accessProfile(source.data(), width, height, stride, quarter, limited);
					Image<ColorRgb> image;
					const uint8_t* lastBuffer = nullptr;
					for (int i = 0; i < options.warmup + options.iterations; ++i)
					{
						const auto start = std::chrono::steady_clock::now();
						FrameDecoder::dispatchProcessImageVector[quarter][tone][false](0, 0, 0, 0,
							source.data(), nullptr, width, height, stride,
							limited ? PixelFormat::BGR24_LIMITED : PixelFormat::BGR24, lut.data(), image, nullptr);
						const auto end = std::chrono::steady_clock::now();
						const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
						if (i == 0) c.firstNs = ns;
						if (i >= options.warmup)
						{
							if (lastBuffer && image.rawMem() != lastBuffer) ++c.outputReallocations;
							c.samplesNs.push_back(ns);
						}
						lastBuffer = image.rawMem();
					}
					c.outputWidth = int(image.width());
					c.outputHeight = int(image.height());
					run.cases.push_back(std::move(c));
				}
	}
	run.meta.after = BenchReport::snapshot(BenchReport::SysRoot{}, run.meta.pinnedCpu);

	std::fputs(BenchReport::text(run, options.iterations, options.warmup).c_str(), stdout);
	int status = 0;
	auto write = [&](const std::optional<std::string>& path, const std::string& body) {
		if (!path) return;
		std::FILE* f = std::fopen(path->c_str(), "wb");
		if (!f || std::fwrite(body.data(), 1, body.size(), f) != body.size()) { std::fprintf(stderr, "bgr24-bench: cannot write %s\n", path->c_str()); status = 1; }
		if (f && std::fclose(f) != 0) status = 1;
	};
	write(options.jsonPath, BenchReport::json(run, options.raw));
	write(options.csvPath, BenchReport::csv(run));
	return status;
}
