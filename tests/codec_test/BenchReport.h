#pragma once
// Qt-free reporting for `DecoderTest --bgr24-bench` (issue #12).
//
// Everything here is deterministic and unit-tested by tests/bench_report without
// Qt: statistics, run metadata, the LUT access profile of an input, and the text,
// JSON and CSV writers. Timing itself stays in Bgr24Checks.h.
//
// Missing is not zero. Every metadata field that may be unavailable is an
// std::optional and is written as JSON null / CSV "NA"; an observed 0 is written
// as 0.

#include <utils/Bgr24.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef __linux__
	#include <sched.h>
#endif

namespace BenchReport
{
inline constexpr const char* schema = "hyperhdr.bgr24-bench/1";

// ---------------------------------------------------------------- statistics

// Percentile = element floor(n * p / 100) (0-based) of the ascending samples,
// clamped to the last element. This is the rule the original text table used
// (median = s[n/2], p95 = s[n*95/100]), kept so old and new output agree.
inline int64_t rankPercentile(const std::vector<int64_t>& sorted, int p)
{
	const size_t i = std::min(sorted.size() - 1, (sorted.size() * size_t(p)) / 100);
	return sorted[i];
}

struct Summary
{
	size_t count = 0;
	int64_t minNs = 0, maxNs = 0, medianNs = 0, p95Ns = 0, p99Ns = 0;
	double meanNs = 0;
	std::optional<double> stddevNs;   // sample standard deviation; absent below two samples
};

// Requires at least one sample.
inline Summary summarize(std::vector<int64_t> samples)
{
	std::sort(samples.begin(), samples.end());
	Summary s;
	s.count = samples.size();
	s.minNs = samples.front();
	s.maxNs = samples.back();
	s.medianNs = rankPercentile(samples, 50);
	s.p95Ns = rankPercentile(samples, 95);
	s.p99Ns = rankPercentile(samples, 99);
	long double sum = 0;
	for (auto v : samples) sum += v;
	s.meanNs = double(sum / samples.size());
	if (samples.size() > 1)
	{
		long double sq = 0;
		for (auto v : samples) { const long double d = v - (long double)s.meanNs; sq += d * d; }
		s.stddevNs = double(std::sqrt(sq / (samples.size() - 1)));
	}
	return s;
}

// ------------------------------------------------------- LUT access profile

// What the LUT lookups of one decode touch, computed from the input itself (no
// timing). Uses the same sampling (every second pixel and row for quarter
// output) and the same range expansion as Bgr24::pixel, so it predicts the
// memory footprint the timed decode really has.
struct AccessProfile
{
	uint64_t lookups = 0;
	uint64_t distinctEntries = 0;   // distinct RGB triples looked up
	uint64_t distinctLines = 0;     // distinct 64-byte LUT cache lines
	uint64_t distinctPages = 0;     // distinct 4 KiB LUT pages
};

inline AccessProfile accessProfile(const uint8_t* data, int width, int height, int stride, bool quarter, bool limited)
{
	const int step = quarter ? 2 : 1;
	const int outW = quarter ? width / 2 : width, outH = quarter ? height / 2 : height;
	std::vector<uint8_t> entry(size_t(1) << 24, 0);
	std::vector<uint8_t> line((size_t(1) << 24) * 3 / 64 + 1, 0);
	std::vector<uint8_t> page((size_t(1) << 24) * 3 / 4096 + 1, 0);
	AccessProfile p;
	for (int y = 0; y < outH; ++y)
	{
		const uint8_t* s = data + size_t(stride) * size_t(y * step);
		for (int x = 0; x < outW; ++x, s += 3 * step)
		{
			const uint8_t r = limited ? Bgr24::limitedTable[s[2]] : s[2];
			const uint8_t g = limited ? Bgr24::limitedTable[s[1]] : s[1];
			const uint8_t b = limited ? Bgr24::limitedTable[s[0]] : s[0];
			const uint32_t e = uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16);
			const uint64_t byte = uint64_t(e) * 3;
			++p.lookups;
			if (!entry[e]) { entry[e] = 1; ++p.distinctEntries; }
			// An entry's three bytes can straddle a line or page boundary.
			for (uint64_t at : { byte, byte + 2 })
			{
				if (!line[at / 64]) { line[at / 64] = 1; ++p.distinctLines; }
				if (!page[at / 4096]) { page[at / 4096] = 1; ++p.distinctPages; }
			}
		}
	}
	return p;
}

// ---------------------------------------------------------------- metadata

struct Thermal
{
	std::string zone;                 // thermal_zoneN
	std::optional<std::string> type;  // e.g. bigcore0-thermal
	std::optional<int64_t> milliC;
};

// Readings that change over a run: taken before the first and after the last case.
struct Snapshot
{
	std::optional<int64_t> curFreqKhz;
	std::optional<std::vector<double>> loadavg;   // 1, 5, 15 minutes
	std::vector<Thermal> thermal;                 // empty when no zone is readable
};

struct Metadata
{
	std::optional<std::string> commit, label, compiler, buildType, os, arch;
	bool optimized = false, assertions = false;
	std::optional<std::vector<int>> affinity;   // CPUs the process may run on
	std::optional<int> pinnedCpu;               // the CPU when the affinity is exactly one
	std::optional<int> startCpu;                // where it ran at start (meaningful only when pinned)
	std::optional<std::string> midr, core, modelName;
	std::optional<std::string> governor;
	std::optional<int64_t> minFreqKhz, maxFreqKhz;
	Snapshot before, after;
};

// File system access relative to a root ("" for the real system), so tests can
// supply a fake /sys and /proc.
struct SysRoot
{
	std::string root;

	std::optional<std::string> text(const std::string& path) const
	{
		std::ifstream f(root + path);
		if (!f) return std::nullopt;
		std::stringstream ss; ss << f.rdbuf();
		std::string s = ss.str();
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
		if (s.empty()) return std::nullopt;
		return s;
	}

	std::optional<int64_t> integer(const std::string& path) const
	{
		const auto t = text(path);
		if (!t) return std::nullopt;
		char* end = nullptr;
		const long long v = std::strtoll(t->c_str(), &end, 10);
		if (end == t->c_str() || *end != '\0') return std::nullopt;   // non-numeric is missing, not zero
		return v;
	}
};

// MIDR_EL1 part numbers for the cores the RK3588 has (and its neighbours).
inline std::optional<std::string> coreName(const std::string& midrHex)
{
	char* end = nullptr;
	const unsigned long long midr = std::strtoull(midrHex.c_str(), &end, 16);
	if (end == midrHex.c_str()) return std::nullopt;
	const unsigned implementer = (midr >> 24) & 0xff, part = (midr >> 4) & 0xfff;
	if (implementer != 0x41) return std::nullopt;   // ARM Ltd.
	switch (part)
	{
		case 0xd03: return "Cortex-A53";
		case 0xd05: return "Cortex-A55";
		case 0xd08: return "Cortex-A72";
		case 0xd0b: return "Cortex-A76";
		case 0xd41: return "Cortex-A78";
		default: return std::nullopt;
	}
}

inline Snapshot snapshot(const SysRoot& sys, std::optional<int> cpu)
{
	Snapshot s;
	if (cpu) s.curFreqKhz = sys.integer("/sys/devices/system/cpu/cpu" + std::to_string(*cpu) + "/cpufreq/scaling_cur_freq");
	if (const auto l = sys.text("/proc/loadavg"))
	{
		std::istringstream in(*l);
		std::vector<double> v(3);
		if (in >> v[0] >> v[1] >> v[2]) s.loadavg = v;
	}
	for (int z = 0; z < 64; ++z)
	{
		const std::string dir = "/sys/class/thermal/thermal_zone" + std::to_string(z);
		auto type = sys.text(dir + "/type");
		auto temp = sys.integer(dir + "/temp");
		if (!type && !temp) continue;
		s.thermal.push_back({ "thermal_zone" + std::to_string(z), type, temp });
	}
	return s;
}

// The parts that need the live process: CPU affinity and the current CPU.
inline std::optional<std::vector<int>> processAffinity()
{
#ifdef __linux__
	cpu_set_t set;
	CPU_ZERO(&set);
	if (sched_getaffinity(0, sizeof(set), &set) != 0) return std::nullopt;
	std::vector<int> cpus;
	for (int c = 0; c < CPU_SETSIZE; ++c)
		if (CPU_ISSET(c, &set)) cpus.push_back(c);
	return cpus;
#else
	return std::nullopt;
#endif
}

inline std::optional<int> currentCpu()
{
#ifdef __linux__
	const int c = sched_getcpu();
	if (c >= 0) return c;
#endif
	return std::nullopt;
}

inline std::string compilerString()
{
#if defined(__clang__)
	return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
	return std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
	return "msvc " + std::to_string(_MSC_VER);
#else
	return "unknown";
#endif
}

inline std::optional<std::string> osString()
{
#if defined(__linux__)
	return "linux";
#elif defined(_WIN32)
	return "windows";
#elif defined(__APPLE__)
	return "macos";
#else
	return std::nullopt;
#endif
}

inline std::optional<std::string> archString()
{
#if defined(__aarch64__) || defined(_M_ARM64)
	return "aarch64";
#elif defined(__arm__)
	return "arm";
#elif defined(__x86_64__) || defined(_M_X64)
	return "x86_64";
#else
	return std::nullopt;
#endif
}

inline Metadata collect(const SysRoot& sys, std::optional<std::vector<int>> affinity, std::optional<int> startCpu)
{
	Metadata m;
	m.compiler = compilerString();
	m.os = osString();
	m.arch = archString();
#ifdef HYPERHDR_BENCH_BUILD_TYPE
	if (std::string(HYPERHDR_BENCH_BUILD_TYPE).size()) m.buildType = HYPERHDR_BENCH_BUILD_TYPE;
#endif
#if defined(__OPTIMIZE__) || (defined(_MSC_VER) && !defined(_DEBUG))
	m.optimized = true;
#endif
#ifndef NDEBUG
	m.assertions = true;
#endif
	m.affinity = affinity;
	if (affinity && affinity->size() == 1) m.pinnedCpu = affinity->front();
	m.startCpu = startCpu;
	// Per-core sysfs only describes the core we run on when we are pinned to it.
	const std::optional<int> cpu = m.pinnedCpu;
	if (cpu)
	{
		const std::string d = "/sys/devices/system/cpu/cpu" + std::to_string(*cpu);
		m.midr = sys.text(d + "/regs/identification/midr_el1");
		if (m.midr) m.core = coreName(*m.midr);
		m.governor = sys.text(d + "/cpufreq/scaling_governor");
		m.minFreqKhz = sys.integer(d + "/cpufreq/scaling_min_freq");
		m.maxFreqKhz = sys.integer(d + "/cpufreq/scaling_max_freq");
	}
	if (const auto info = sys.text("/proc/cpuinfo"))
	{
		std::istringstream in(*info);
		for (std::string line; std::getline(in, line);)
			if (line.rfind("model name", 0) == 0 && line.find(':') != std::string::npos)
			{
				auto v = line.substr(line.find(':') + 1);
				v.erase(0, v.find_first_not_of(" \t"));
				if (!v.empty()) m.modelName = v;
				break;
			}
	}
	m.before = snapshot(sys, cpu);
	return m;
}

// ---------------------------------------------------------------- results

struct Case
{
	std::string range;     // full | limited
	std::string output;    // full | quarter
	bool lut = false;
	std::string pattern;   // random | gradient | solid
	int outputWidth = 0, outputHeight = 0;
	int warmup = 0;
	int64_t firstNs = 0;                   // the first (cold) call, part of the warm-up
	unsigned outputReallocations = 0;      // output buffer address changes across timed calls
	std::vector<int64_t> samplesNs;
	std::optional<AccessProfile> profile;  // LUT cases only

	std::string scenario() const { return range + "-" + output + "-" + (lut ? "lut" : "nolut") + "-" + pattern; }
};

// Where the benchmark input came from. Synthetic patterns are generated in
// memory; a file input is a saved raw frame with its description (RawFrameInput.h).
struct Input
{
	std::string kind = "synthetic";   // synthetic | file
	std::optional<std::string> name, sha256, format, declaredRange, orientation, source;
	std::optional<int64_t> payloadBytes;
};

struct Run
{
	int width = 0, height = 0, stride = 0;
	uint64_t lutBytes = 0;
	std::string lutFill;
	uint32_t seed = 0;
	Metadata meta;
	std::vector<Case> cases;
	Input input;   // last, so existing aggregate initialisers keep their meaning
};

// ---------------------------------------------------------------- writers

inline std::string ms3(int64_t ns) { char b[32]; std::snprintf(b, sizeof b, "%.3f", ns / 1e6); return b; }

// The original human-readable table, unchanged for the default (random) pattern.
// Other patterns get their own table under an "input pattern" line.
inline std::string text(const Run& run, int iterations, int warmup)
{
	std::string out;
	char b[256];
	std::snprintf(b, sizeof b, "BGR24 decode %dx%d stride %d: %d timed runs per case after %d warm-up (ms)\n",
		run.width, run.height, run.stride, iterations, warmup);
	out += b;
	if (run.input.kind == "file")
		out += "input file: " + run.input.name.value_or("?") + " sha256 " + run.input.sha256.value_or("?") +
			" (declared range " + run.input.declaredRange.value_or("?") + ")\n";
	std::string pattern;
	for (const auto& c : run.cases)
	{
		if (c.pattern != pattern)
		{
			if (!pattern.empty() || c.pattern != "random")
				out += "input pattern: " + c.pattern + "\n";
			pattern = c.pattern;
			std::snprintf(b, sizeof b, "%-8s %-8s %-4s %10s %10s %10s\n", "range", "output", "lut", "median", "p95", "min");
			out += b;
		}
		const auto s = summarize(c.samplesNs);
		std::snprintf(b, sizeof b, "%-8s %-8s %-4s %10.3f %10.3f %10.3f\n", c.range.c_str(), c.output.c_str(), c.lut ? "yes" : "no",
			s.medianNs / 1e6, s.p95Ns / 1e6, s.minNs / 1e6);
		out += b;
	}
	return out;
}

inline std::string jsonString(const std::string& s)
{
	std::string o = "\"";
	for (unsigned char ch : s)
	{
		if (ch == '"' || ch == '\\') { o += '\\'; o += char(ch); }
		else if (ch == '\n') o += "\\n";
		else if (ch == '\t') o += "\\t";
		else if (ch < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
		else o += char(ch);
	}
	return o + "\"";
}

template<typename T> std::string jsonValue(const std::optional<T>& v);
inline std::string jsonValue(const std::string& v) { return jsonString(v); }
inline std::string jsonValue(int64_t v) { return std::to_string(v); }
inline std::string jsonValue(int v) { return std::to_string(v); }
inline std::string jsonValue(double v)
{
	if (!std::isfinite(v)) return "null";
	char b[40]; std::snprintf(b, sizeof b, "%.6f", v); return b;
}
template<typename T> std::string jsonValue(const std::vector<T>& v)
{
	std::string o = "[";
	for (size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + jsonValue(v[i]);
	return o + "]";
}
template<typename T> std::string jsonValue(const std::optional<T>& v) { return v ? jsonValue(*v) : "null"; }

inline std::string jsonSnapshot(const Snapshot& s)
{
	std::string o = "{\"cur_freq_khz\":" + jsonValue(s.curFreqKhz) + ",\"loadavg\":" + jsonValue(s.loadavg) + ",\"thermal\":[";
	for (size_t i = 0; i < s.thermal.size(); ++i)
		o += std::string(i ? "," : "") + "{\"zone\":" + jsonString(s.thermal[i].zone) + ",\"type\":" + jsonValue(s.thermal[i].type) +
			",\"milli_c\":" + jsonValue(s.thermal[i].milliC) + "}";
	return o + "]}";
}

inline std::string json(const Run& run, bool raw)
{
	const auto& m = run.meta;
	std::string o = "{\n";
	o += "  \"schema\": " + jsonString(schema) + ",\n";
	o += "  \"metadata\": {\"commit\":" + jsonValue(m.commit) + ",\"label\":" + jsonValue(m.label) +
		",\"compiler\":" + jsonValue(m.compiler) + ",\"build_type\":" + jsonValue(m.buildType) +
		",\"optimized\":" + (m.optimized ? "true" : "false") + ",\"assertions\":" + (m.assertions ? "true" : "false") +
		",\"os\":" + jsonValue(m.os) + ",\"arch\":" + jsonValue(m.arch) +
		",\"affinity\":" + jsonValue(m.affinity) + ",\"pinned_cpu\":" + jsonValue(m.pinnedCpu) + ",\"start_cpu\":" + jsonValue(m.startCpu) +
		",\"midr\":" + jsonValue(m.midr) + ",\"core\":" + jsonValue(m.core) + ",\"model_name\":" + jsonValue(m.modelName) +
		",\"governor\":" + jsonValue(m.governor) + ",\"min_freq_khz\":" + jsonValue(m.minFreqKhz) + ",\"max_freq_khz\":" + jsonValue(m.maxFreqKhz) +
		",\n    \"before\":" + jsonSnapshot(m.before) + ",\n    \"after\":" + jsonSnapshot(m.after) + "},\n";
	o += "  \"config\": {\"width\":" + std::to_string(run.width) + ",\"height\":" + std::to_string(run.height) +
		",\"stride\":" + std::to_string(run.stride) + ",\"lut_bytes\":" + std::to_string(run.lutBytes) +
		",\"lut_fill\":" + jsonString(run.lutFill) + ",\"seed\":" + std::to_string(run.seed) +
		",\"percentile_rule\":\"sorted[floor(n*p/100)]\",\"time_unit\":\"ns\",\"clock\":\"steady_clock\"" +
		",\n    \"input\":{\"kind\":" + jsonString(run.input.kind) + ",\"name\":" + jsonValue(run.input.name) + ",\"sha256\":" + jsonValue(run.input.sha256) +
		",\"format\":" + jsonValue(run.input.format) + ",\"declared_range\":" + jsonValue(run.input.declaredRange) +
		",\"orientation\":" + jsonValue(run.input.orientation) + ",\"payload_bytes\":" + jsonValue(run.input.payloadBytes) +
		",\"source\":" + jsonValue(run.input.source) + "}},\n";
	o += "  \"cases\": [";
	for (size_t i = 0; i < run.cases.size(); ++i)
	{
		const auto& c = run.cases[i];
		const auto s = summarize(c.samplesNs);
		o += std::string(i ? "," : "") + "\n    {\"scenario\":" + jsonString(c.scenario()) + ",\"range\":" + jsonString(c.range) +
			",\"output\":" + jsonString(c.output) + ",\"lut\":" + (c.lut ? "true" : "false") + ",\"pattern\":" + jsonString(c.pattern) +
			",\"output_width\":" + std::to_string(c.outputWidth) + ",\"output_height\":" + std::to_string(c.outputHeight) +
			",\"warmup\":" + std::to_string(c.warmup) + ",\"samples\":" + std::to_string(s.count) +
			",\"first_ns\":" + std::to_string(c.firstNs) + ",\"output_reallocations\":" + std::to_string(c.outputReallocations) +
			",\"min_ns\":" + std::to_string(s.minNs) + ",\"median_ns\":" + std::to_string(s.medianNs) +
			",\"p95_ns\":" + std::to_string(s.p95Ns) + ",\"p99_ns\":" + std::to_string(s.p99Ns) + ",\"max_ns\":" + std::to_string(s.maxNs) +
			",\"mean_ns\":" + jsonValue(s.meanNs) + ",\"stddev_ns\":" + jsonValue(s.stddevNs);
		if (c.profile)
			o += ",\"lut_access\":{\"lookups\":" + std::to_string(c.profile->lookups) + ",\"distinct_entries\":" + std::to_string(c.profile->distinctEntries) +
				",\"distinct_lines_64\":" + std::to_string(c.profile->distinctLines) + ",\"distinct_pages_4k\":" + std::to_string(c.profile->distinctPages) + "}";
		else
			o += ",\"lut_access\":null";
		if (raw) o += ",\"raw_ns\":" + jsonValue(c.samplesNs);
		o += "}";
	}
	return o + "\n  ]\n}\n";
}

inline std::string csvField(const std::optional<std::string>& v)
{
	if (!v) return "NA";
	if (v->find_first_of(",\"\n") == std::string::npos) return *v;
	std::string o = "\"";
	for (char ch : *v) { if (ch == '"') o += '"'; o += ch; }
	return o + "\"";
}
template<typename T> std::string csvNum(const std::optional<T>& v) { return v ? std::to_string(*v) : "NA"; }

inline const char* csvHeader =
	"schema,commit,label,core,pinned_cpu,governor,freq_khz_before,freq_khz_after,max_temp_mc_before,max_temp_mc_after,"
	"scenario,range,output,lut,pattern,output_width,output_height,warmup,samples,min_ns,median_ns,p95_ns,p99_ns,max_ns,mean_ns,lut_distinct_lines_64\n";

inline std::optional<int64_t> maxTemp(const Snapshot& s)
{
	std::optional<int64_t> t;
	for (const auto& z : s.thermal)
		if (z.milliC && (!t || *z.milliC > *t)) t = z.milliC;
	return t;
}

inline std::string csv(const Run& run)
{
	const auto& m = run.meta;
	std::string o = csvHeader;
	for (const auto& c : run.cases)
	{
		const auto s = summarize(c.samplesNs);
		char mean[40]; std::snprintf(mean, sizeof mean, "%.1f", s.meanNs);
		o += std::string(schema) + "," + csvField(m.commit) + "," + csvField(m.label) + "," + csvField(m.core) + "," + csvNum(m.pinnedCpu) + "," +
			csvField(m.governor) + "," + csvNum(m.before.curFreqKhz) + "," + csvNum(m.after.curFreqKhz) + "," +
			csvNum(maxTemp(m.before)) + "," + csvNum(maxTemp(m.after)) + "," +
			c.scenario() + "," + c.range + "," + c.output + "," + (c.lut ? "yes" : "no") + "," + c.pattern + "," +
			std::to_string(c.outputWidth) + "," + std::to_string(c.outputHeight) + "," + std::to_string(c.warmup) + "," +
			std::to_string(s.count) + "," + std::to_string(s.minNs) + "," + std::to_string(s.medianNs) + "," +
			std::to_string(s.p95Ns) + "," + std::to_string(s.p99Ns) + "," + std::to_string(s.maxNs) + "," + mean + "," +
			(c.profile ? std::to_string(c.profile->distinctLines) : std::string("NA")) + "\n";
	}
	return o;
}

// ---------------------------------------------------------------- inputs

// The original benchmark input: an LCG over every byte (seed 12345). Kept
// byte-for-byte so "random" results stay comparable with earlier runs.
inline void fillRandom(std::vector<uint8_t>& source, uint32_t seed)
{
	for (auto& b : source) { seed = seed * 1664525u + 1013904223u; b = uint8_t(seed >> 24); }
}

// Spatially coherent content: neighbouring pixels differ by at most a few codes,
// like most video, so consecutive LUT lookups land close together.
inline void fillGradient(std::vector<uint8_t>& source, int width, int height, int stride)
{
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			uint8_t* p = source.data() + size_t(y) * stride + size_t(x) * 3;
			p[0] = uint8_t((x * 255) / std::max(1, width - 1));             // B
			p[1] = uint8_t((y * 255) / std::max(1, height - 1));            // G
			p[2] = uint8_t(((x + y) * 255) / std::max(1, width + height - 2)); // R
		}
}

// One colour everywhere: every lookup hits the same LUT entry (best case).
inline void fillSolid(std::vector<uint8_t>& source, int width, int height, int stride)
{
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			uint8_t* p = source.data() + size_t(y) * stride + size_t(x) * 3;
			p[0] = 0x80; p[1] = 0x80; p[2] = 0x80;
		}
}

inline bool fillPattern(const std::string& pattern, std::vector<uint8_t>& source, int width, int height, int stride, uint32_t seed)
{
	if (pattern == "random") fillRandom(source, seed);
	else if (pattern == "gradient") fillGradient(source, width, height, stride);
	else if (pattern == "solid") fillSolid(source, width, height, stride);
	else return false;
	return true;
}

// ---------------------------------------------------------------- options

struct Options
{
	int iterations = 200, warmup = 10;
	std::vector<std::string> patterns{ "random" };
	std::optional<std::string> jsonPath, csvPath, commit, label;
	std::optional<std::string> inputPath, inputDescription;   // --input / --input-desc
	bool raw = false;
	std::string error;   // non-empty: the command line was rejected
};

// Parses the arguments after --bgr24-bench. Unknown options are errors so a typo
// can't silently produce a default run.
inline Options parseOptions(const std::vector<std::string>& args)
{
	Options o;
	bool patternGiven = false;
	auto positive = [&](const std::string& v, int& out, const char* name) {
		char* end = nullptr;
		const long n = std::strtol(v.c_str(), &end, 10);
		if (end == v.c_str() || *end || n < 1 || n > 1000000) { o.error = std::string("invalid ") + name + ": " + v; return; }
		out = int(n);
	};
	for (size_t i = 0; i < args.size() && o.error.empty(); ++i)   // report the first problem
	{
		const std::string& a = args[i];
		auto value = [&](const char* name) -> std::optional<std::string> {
			const std::string prefix = std::string(name) + "=";
			if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
			if (a == name && i + 1 < args.size()) return args[++i];
			return std::nullopt;
		};
		if (a == "--bgr24-bench") continue;
		if (a == "--raw") { o.raw = true; continue; }
		if (auto v = value("--json")) { o.jsonPath = v; continue; }
		if (auto v = value("--csv")) { o.csvPath = v; continue; }
		if (auto v = value("--commit")) { if (!v->empty()) o.commit = v; continue; }
		if (auto v = value("--label")) { if (!v->empty()) o.label = v; continue; }
		if (auto v = value("--iterations")) { positive(*v, o.iterations, "--iterations"); continue; }
		if (auto v = value("--warmup")) { positive(*v, o.warmup, "--warmup"); continue; }
		if (auto v = value("--input")) { o.inputPath = v; continue; }
		if (auto v = value("--input-desc")) { o.inputDescription = v; continue; }
		if (auto v = value("--pattern"))
		{
			patternGiven = true;
			o.patterns.clear();
			std::stringstream ss(*v);
			for (std::string p; std::getline(ss, p, ',');)
			{
				if (p != "random" && p != "gradient" && p != "solid") { o.error = "unknown --pattern: " + p; break; }
				if (std::find(o.patterns.begin(), o.patterns.end(), p) == o.patterns.end()) o.patterns.push_back(p);
			}
			if (o.patterns.empty() && o.error.empty()) o.error = "empty --pattern";
			continue;
		}
		o.error = "unknown benchmark option: " + a;
	}
	if (o.error.empty() && o.inputPath.has_value() != o.inputDescription.has_value())
		o.error = "--input and --input-desc must be given together";
	if (o.error.empty() && o.inputPath && patternGiven)
		o.error = "--input replaces --pattern; give one or the other";
	if (o.error.empty() && o.inputPath)
		o.patterns.clear();   // the file is the only input
	return o;
}
}
