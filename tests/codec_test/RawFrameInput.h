#pragma once
// Saved raw frames as benchmark input (`DecoderTest --bgr24-bench --input`,
// issues #9 and #12). Qt-free and unit-tested by tests/bench_report.
//
// A frame is two files: the raw bytes, exactly as a capture buffer holds them,
// and a description written by a person. Nothing about the frame is guessed from
// its size; every property the decoder depends on is stated, and the file must
// agree with the statement. Format: docs/rk3588-benchmark-reporting.md
// ("Captured-frame input"). Example:
//
//   # hyperhdr raw frame description, version 1
//   format = BGR24
//   width = 1920
//   height = 1080
//   stride = 5760
//   range = limited
//   orientation = top-down
//   payload = 6220800
//   source = synthetic gradient, no personal content
//
// The benchmark never reads a capture device. Only BGR24, top-down rows (the
// first byte is the blue value of the top-left pixel) is supported; anything else
// is rejected rather than reinterpreted.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace RawFrame
{
inline constexpr int maxWidth = 7680, maxHeight = 4320;

struct Description
{
	std::string format;        // BGR24
	int width = 0, height = 0;
	int stride = 0;            // bytes from one row to the next
	std::string range;         // full | limited: the content's quantization as captured
	std::string orientation;   // top-down
	uint64_t payload = 0;      // bytes in the raw file
	std::optional<std::string> source;   // free text, e.g. how it was produced

	// Bytes the decoder reads: every row but the last at full stride, then the
	// last row's pixels. A capture buffer usually also holds the last row's padding.
	uint64_t minimumPayload() const { return uint64_t(height - 1) * uint64_t(stride) + uint64_t(width) * 3; }
	uint64_t paddedPayload() const { return uint64_t(height) * uint64_t(stride); }
};

struct Parsed
{
	std::optional<Description> description;
	std::string error;
};

inline std::string trim(const std::string& s)
{
	const auto b = s.find_first_not_of(" \t\r");
	if (b == std::string::npos) return {};
	const auto e = s.find_last_not_of(" \t\r");
	return s.substr(b, e - b + 1);
}

inline bool integer(const std::string& v, int64_t lo, int64_t hi, int64_t& out)
{
	if (v.empty() || v.size() > 12) return false;
	for (char c : v) if (c < '0' || c > '9') return false;
	const int64_t n = std::stoll(v);
	if (n < lo || n > hi) return false;
	out = n;
	return true;
}

// Parses and checks a description on its own (the raw file isn't needed yet).
inline Parsed parseDescription(const std::string& text)
{
	static const std::set<std::string> required{ "format", "width", "height", "stride", "range", "orientation", "payload" };
	std::map<std::string, std::string> kv;
	std::istringstream in(text);
	int lineNo = 0;
	for (std::string line; std::getline(in, line);)
	{
		++lineNo;
		const std::string t = trim(line);
		if (t.empty() || t[0] == '#') continue;
		const auto eq = t.find('=');
		if (eq == std::string::npos) return { std::nullopt, "line " + std::to_string(lineNo) + ": expected key = value" };
		const std::string key = trim(t.substr(0, eq)), value = trim(t.substr(eq + 1));
		if (!required.count(key) && key != "source") return { std::nullopt, "line " + std::to_string(lineNo) + ": unknown key '" + key + "'" };
		if (kv.count(key)) return { std::nullopt, "line " + std::to_string(lineNo) + ": duplicate key '" + key + "'" };
		if (value.empty()) return { std::nullopt, "line " + std::to_string(lineNo) + ": empty value for '" + key + "'" };
		kv[key] = value;
	}
	for (const auto& k : required)
		if (!kv.count(k)) return { std::nullopt, "missing key '" + k + "'" };

	Description d;
	d.format = kv["format"];
	if (d.format != "BGR24") return { std::nullopt, "unsupported format '" + d.format + "' (only BGR24)" };
	int64_t n = 0;
	if (!integer(kv["width"], 2, maxWidth, n)) return { std::nullopt, "width must be 2.." + std::to_string(maxWidth) };
	d.width = int(n);
	if (!integer(kv["height"], 2, maxHeight, n)) return { std::nullopt, "height must be 2.." + std::to_string(maxHeight) };
	d.height = int(n);
	if (!integer(kv["stride"], 1, int64_t(maxWidth) * 4, n)) return { std::nullopt, "stride is not a valid byte count" };
	d.stride = int(n);
	if (d.stride < d.width * 3) return { std::nullopt, "stride " + std::to_string(d.stride) + " is less than width x 3 = " + std::to_string(d.width * 3) };
	d.range = kv["range"];
	if (d.range != "full" && d.range != "limited") return { std::nullopt, "range must be full or limited" };
	d.orientation = kv["orientation"];
	if (d.orientation != "top-down") return { std::nullopt, "unsupported orientation '" + d.orientation + "' (only top-down)" };
	if (!integer(kv["payload"], 1, int64_t(maxHeight) * maxWidth * 4, n)) return { std::nullopt, "payload is not a valid byte count" };
	d.payload = uint64_t(n);
	if (d.payload != d.minimumPayload() && d.payload != d.paddedPayload())
		return { std::nullopt, "payload " + std::to_string(d.payload) + " matches neither height x stride = " + std::to_string(d.paddedPayload()) +
			" nor the unpadded last row = " + std::to_string(d.minimumPayload()) };
	if (kv.count("source")) d.source = kv["source"];
	return { d, {} };
}

// Lower-case hex SHA-256 (FIPS 180-4), to identify the input in results.
inline std::string sha256(const std::vector<uint8_t>& data)
{
	static const uint32_t k[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
		0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
		0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
		0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
	uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
	std::vector<uint8_t> m(data);
	const uint64_t bits = uint64_t(data.size()) * 8;
	m.push_back(0x80);
	while (m.size() % 64 != 56) m.push_back(0);
	for (int i = 7; i >= 0; --i) m.push_back(uint8_t(bits >> (i * 8)));
	for (size_t off = 0; off < m.size(); off += 64)
	{
		uint32_t w[64];
		for (int i = 0; i < 16; ++i)
			w[i] = (uint32_t(m[off + i * 4]) << 24) | (uint32_t(m[off + i * 4 + 1]) << 16) | (uint32_t(m[off + i * 4 + 2]) << 8) | m[off + i * 4 + 3];
		for (int i = 16; i < 64; ++i)
		{
			const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i < 64; ++i)
		{
			const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
			const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
	}
	char out[65];
	for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
	return std::string(out, 64);
}

struct Frame
{
	Description description;
	std::vector<uint8_t> bytes;   // exactly payload bytes
	std::string sha256;
	std::string name;             // file name without directories
};

struct Loaded
{
	std::optional<Frame> frame;
	std::string error;
};

inline std::string baseName(const std::string& path)
{
	const auto slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Reads and checks both files. The raw file must be exactly payload bytes: a
// short file (truncated capture) or a longer one (different layout) is rejected.
inline Loaded load(const std::string& rawPath, const std::string& descriptionPath)
{
	std::ifstream df(descriptionPath);
	if (!df) return { std::nullopt, "cannot read description " + descriptionPath };
	const std::string text((std::istreambuf_iterator<char>(df)), std::istreambuf_iterator<char>());
	auto parsed = parseDescription(text);
	if (!parsed.description) return { std::nullopt, descriptionPath + ": " + parsed.error };
	const Description& d = *parsed.description;

	std::ifstream rf(rawPath, std::ios::binary | std::ios::ate);
	if (!rf) return { std::nullopt, "cannot read input " + rawPath };
	const auto size = rf.tellg();
	if (size < 0) return { std::nullopt, "cannot size input " + rawPath };
	if (uint64_t(size) != d.payload)
		return { std::nullopt, rawPath + " is " + std::to_string(uint64_t(size)) + " bytes; the description says payload " + std::to_string(d.payload) +
			(uint64_t(size) < d.payload ? " (short file)" : " (longer than described)") };
	rf.seekg(0);
	Frame frame;
	frame.description = d;
	frame.bytes.resize(size_t(d.payload));
	if (!rf.read(reinterpret_cast<char*>(frame.bytes.data()), std::streamsize(d.payload)))
		return { std::nullopt, "cannot read " + rawPath };
	frame.sha256 = sha256(frame.bytes);
	frame.name = baseName(rawPath);
	return { std::move(frame), {} };
}

// Benchmark pattern name for a file input: stable across runs of the same bytes,
// distinct for different files, so the summarizer never groups two inputs.
inline std::string patternName(const Frame& f)
{
	return "file:" + f.sha256.substr(0, 12);
}
}
