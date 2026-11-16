// Qt-free tests for tests/codec_test/BenchReport.h (benchmark statistics,
// metadata, LUT access profile and output formats). Expected values are worked
// out by hand or by an independent formula, not by calling the code under test.
//
//   bench-report-test                  run the tests
//   bench-report-test --emit-sample D  also write a synthetic sample run to D
//                                      (consumed by tests/ci/bench-summarize-test.py)
#include "BenchReport.h"
#include "RawFrameInput.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;
using namespace BenchReport;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " #cond "\n"; } } while (0)

static bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

static void write(const fs::path& p, const std::string& body)
{
	fs::create_directories(p.parent_path());
	std::ofstream(p) << body;
}

static void statistics()
{
	// One sample: every percentile is that sample, no deviation.
	auto one = summarize({ 7 });
	CHECK(one.count == 1 && one.minNs == 7 && one.medianNs == 7 && one.p95Ns == 7 && one.p99Ns == 7 && one.maxNs == 7);
	CHECK(!one.stddevNs && one.meanNs == 7);

	// 200 samples 1..200 shuffled: floor(200*50/100)=100 -> 101; floor(200*95/100)=190 -> 191; 198 -> 199.
	std::vector<int64_t> v;
	for (int i = 200; i >= 1; --i) v.push_back((i * 7919) % 200 + 1);
	auto s = summarize(v);
	CHECK(s.count == 200 && s.minNs == 1 && s.maxNs == 200);
	CHECK(s.medianNs == 101 && s.p95Ns == 191 && s.p99Ns == 199);
	CHECK(std::abs(s.meanNs - 100.5) < 1e-9);
	// Sample standard deviation of 1..200 = sqrt(200*201/12) = 57.8791845 (Python statistics.stdev).
	CHECK(s.stddevNs && std::abs(*s.stddevNs - 57.8791845) < 1e-6);

	// 7 samples: median index 3, p95 index floor(6.65)=6.
	auto seven = summarize({ 70, 10, 50, 30, 20, 60, 40 });
	CHECK(seven.medianNs == 40 && seven.p95Ns == 70 && seven.p99Ns == 70 && seven.minNs == 10);
	// Two samples: median is the upper one (index 1), as the original table did.
	CHECK(summarize({ 5, 9 }).medianNs == 9);
}

static void accessProfiles()
{
	// Solid mid-grey: one entry, at byte 0x808080*3 = 25264512, a 64-byte boundary.
	{
		const int w = 8, h = 4, stride = 30;
		std::vector<uint8_t> src(size_t(h - 1) * stride + w * 3, 0);
		fillSolid(src, w, h, stride);
		const auto p = accessProfile(src.data(), w, h, stride, false, false);
		CHECK(p.lookups == 32 && p.distinctEntries == 1 && p.distinctLines == 1 && p.distinctPages == 1);
		const auto q = accessProfile(src.data(), w, h, stride, true, false);
		CHECK(q.lookups == 8 && q.distinctEntries == 1);
	}
	// Quarter samples every second pixel and row: only (0,0) and (2,0) of a 4x2 frame.
	{
		const int w = 4, h = 2, stride = 12;
		std::vector<uint8_t> src(size_t(h) * stride, 0);
		auto set = [&](int x, int y, uint8_t b, uint8_t g, uint8_t r) { auto* p = &src[y * stride + x * 3]; p[0] = b; p[1] = g; p[2] = r; };
		set(0, 0, 0, 0, 0);     set(1, 0, 1, 1, 1);
		set(2, 0, 0, 0, 255);   set(3, 0, 3, 3, 3);
		set(0, 1, 9, 9, 9);     set(1, 1, 8, 8, 8); set(2, 1, 7, 7, 7); set(3, 1, 6, 6, 6);
		const auto q = accessProfile(src.data(), w, h, stride, true, false);
		// Entries 0 (bytes 0..2) and 255 (bytes 765..767): lines 0 and 11, both in page 0.
		CHECK(q.lookups == 2 && q.distinctEntries == 2 && q.distinctLines == 2 && q.distinctPages == 1);
		const auto f = accessProfile(src.data(), w, h, stride, false, false);
		CHECK(f.lookups == 8 && f.distinctEntries == 8);
	}
	// Limited range expands before the lookup: 0..16 -> 0 and 235..255 -> 255,
	// so four distinct inputs collapse to two entries.
	{
		const int w = 4, h = 1, stride = 12;
		std::vector<uint8_t> src = { 0,0,0, 16,16,16, 235,235,235, 255,255,255 };
		CHECK(accessProfile(src.data(), w, h, stride, false, false).distinctEntries == 4);
		const auto l = accessProfile(src.data(), w, h, stride, false, true);
		CHECK(l.distinctEntries == 2);
		// White = entry 0xffffff, bytes 50331645..50331647: last line and last page of the 48 MiB table.
		CHECK(l.distinctLines == 2 && l.distinctPages == 2);
	}
	// An entry whose three bytes straddle a line: entry 21 -> bytes 63..65 (lines 0 and 1).
	{
		std::vector<uint8_t> src = { 0, 0, 21 };
		const auto p = accessProfile(src.data(), 1, 1, 3, false, false);
		CHECK(p.distinctEntries == 1 && p.distinctLines == 2 && p.distinctPages == 1);
	}
}

static void inputs()
{
	// The original LCG (seed 12345): first bytes worked out independently.
	std::vector<uint8_t> r(6);
	fillRandom(r, 12345);
	CHECK((r == std::vector<uint8_t>{ 5, 4, 139, 162, 232, 28 }));
	// Gradient: B follows x, G follows y, endpoints reach 0 and 255; padding untouched.
	const int w = 5, h = 3, stride = 17;
	std::vector<uint8_t> g(size_t(h - 1) * stride + w * 3 + 2, 0xee);
	fillGradient(g, w, h, stride);
	CHECK(g[0] == 0 && g[1] == 0 && g[2] == 0);
	CHECK(g[(w - 1) * 3] == 255 && g[size_t(h - 1) * stride + 1] == 255);
	CHECK(g[size_t(h - 1) * stride + (w - 1) * 3 + 2] == 255);
	CHECK(g[15] == 0xee && g[16] == 0xee && g.back() == 0xee);   // row padding / tail
	std::vector<uint8_t> x(3);
	CHECK(!fillPattern("noise", x, 1, 1, 3, 1));
}

static void options()
{
	auto d = parseOptions({ "--bgr24-bench" });
	CHECK(d.error.empty() && d.iterations == 200 && d.warmup == 10 && d.patterns == std::vector<std::string>{ "random" });
	CHECK(!d.jsonPath && !d.csvPath && !d.commit && !d.label && !d.raw);

	auto a = parseOptions({ "--bgr24-bench", "--json=out.json", "--csv", "out.csv", "--raw", "--iterations", "50",
		"--warmup=3", "--pattern", "gradient,random,gradient", "--commit", "abc123", "--label=A76" });
	CHECK(a.error.empty() && a.jsonPath == "out.json" && a.csvPath == "out.csv" && a.raw);
	CHECK(a.iterations == 50 && a.warmup == 3 && a.commit == "abc123" && a.label == "A76");
	CHECK((a.patterns == std::vector<std::string>{ "gradient", "random" }));

	CHECK(!parseOptions({ "--iterations", "0" }).error.empty());
	CHECK(!parseOptions({ "--iterations", "12x" }).error.empty());
	CHECK(!parseOptions({ "--pattern", "noise" }).error.empty());
	CHECK(parseOptions({ "--jsn", "x" }).error == "unknown benchmark option: --jsn");   // a typo is an error, not a default run
	CHECK(parseOptions({ "--iterations", "0", "--pattern", "noise" }).error == "invalid --iterations: 0");
	CHECK(!parseOptions({ "--json" }).error.empty());              // missing value
	CHECK(!parseOptions({ "--commit", "" }).commit);               // empty commit is missing, not ""

	// --input needs its description, replaces the synthetic patterns, and can't
	// be combined with --pattern
	auto in = parseOptions({ "--bgr24-bench", "--input", "f.bgr24", "--input-desc=f.desc" });
	CHECK(in.error.empty() && in.inputPath == "f.bgr24" && in.inputDescription == "f.desc" && in.patterns.empty());
	CHECK(parseOptions({ "--input", "f.bgr24" }).error == "--input and --input-desc must be given together");
	CHECK(parseOptions({ "--input-desc", "f.desc" }).error == "--input and --input-desc must be given together");
	CHECK(parseOptions({ "--input", "f", "--input-desc", "d", "--pattern", "random" }).error == "--input replaces --pattern; give one or the other");
}

static const char* validDescription =
	"# hyperhdr raw frame description, version 1\n"
	"format = BGR24\nwidth = 4\nheight = 3\nstride = 16\nrange = limited\norientation = top-down\npayload = 48\n";

static std::string describe(const std::string& key, const std::string& value)
{
	std::string d = validDescription;
	const auto at = d.find("\n" + key + " = ");
	if (at == std::string::npos) return d + key + " = " + value + "\n";
	const auto end = d.find('\n', at + 1);
	return d.substr(0, at + 1) + (value.empty() ? "" : key + " = " + value) + d.substr(end + (value.empty() ? 1 : 0));
}

static void rawFrames(const fs::path& tmp)
{
	using namespace RawFrame;
	// SHA-256 against the FIPS 180-4 / NIST example vectors
	CHECK(sha256({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(sha256({ 'a', 'b', 'c' }) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";   // two-block message
	CHECK(sha256(std::vector<uint8_t>(two.begin(), two.end())) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

	// A valid description: 4x3 BGR24, stride 16 (4 padding bytes per row)
	auto ok = parseDescription(validDescription);
	CHECK(ok.description && ok.error.empty());
	CHECK(ok.description->minimumPayload() == 2u * 16u + 12u && ok.description->paddedPayload() == 48u);
	CHECK(!ok.description->source);
	CHECK(parseDescription(describe("payload", "44")).description);   // unpadded last row is accepted too

	// Every property is stated, never guessed; anything inconsistent is rejected
	CHECK(parseDescription(describe("format", "")).error == "missing key 'format'");
	CHECK(parseDescription(describe("format", "NV12")).error == "unsupported format 'NV12' (only BGR24)");
	CHECK(parseDescription(describe("orientation", "bottom-up")).error == "unsupported orientation 'bottom-up' (only top-down)");
	CHECK(parseDescription(describe("range", "auto")).error == "range must be full or limited");
	CHECK(contains(parseDescription(describe("stride", "11")).error, "less than width x 3 = 12"));
	CHECK(contains(parseDescription(describe("payload", "47")).error, "matches neither height x stride = 48 nor the unpadded last row = 44"));
	CHECK(contains(parseDescription(describe("width", "4x")).error, "width must be"));
	CHECK(contains(parseDescription(describe("height", "1")).error, "height must be"));
	CHECK(contains(parseDescription(describe("width", "99999")).error, "width must be"));
	CHECK(contains(parseDescription(std::string(validDescription) + "colour = red\n").error, "unknown key 'colour'"));
	CHECK(contains(parseDescription(std::string(validDescription) + "width = 4\n").error, "duplicate key 'width'"));
	CHECK(contains(parseDescription(std::string(validDescription) + "no equals sign\n").error, "expected key = value"));
	CHECK(parseDescription(std::string(validDescription) + "source = synthetic\n").description->source == "synthetic");

	// Files: exactly payload bytes. Short and long files are rejected.
	fs::create_directories(tmp / "raw");
	const auto desc = tmp / "raw/f.desc", rawFile = tmp / "raw/f.bgr24";
	write(desc, validDescription);
	auto bytes = [&](size_t n) {
		std::ofstream f(rawFile, std::ios::binary | std::ios::trunc);
		for (size_t i = 0; i < n; ++i) f.put(char(i));
	};
	bytes(48);
	auto loaded = load(rawFile.string(), desc.string());
	CHECK(loaded.frame && loaded.frame->bytes.size() == 48 && loaded.frame->name == "f.bgr24");
	CHECK(loaded.frame && loaded.frame->bytes[47] == 47);
	CHECK(loaded.frame && patternName(*loaded.frame) == "file:" + loaded.frame->sha256.substr(0, 12) && loaded.frame->sha256.size() == 64);
	bytes(47);
	CHECK(contains(load(rawFile.string(), desc.string()).error, "is 47 bytes; the description says payload 48 (short file)"));
	bytes(49);
	CHECK(contains(load(rawFile.string(), desc.string()).error, "(longer than described)"));
	bytes(0);
	CHECK(contains(load(rawFile.string(), desc.string()).error, "(short file)"));
	CHECK(contains(load((tmp / "raw/none").string(), desc.string()).error, "cannot read input"));
	CHECK(contains(load(rawFile.string(), (tmp / "raw/none.desc").string()).error, "cannot read description"));
	write(desc, describe("format", "RGB24"));
	CHECK(contains(load(rawFile.string(), desc.string()).error, "f.desc: unsupported format"));

	// The committed fixtures load, and their hashes identify them
	const fs::path fixtures = fs::path(BENCH_FIXTURES);
	auto padded = load((fixtures / "scene-64x36-padded.bgr24").string(), (fixtures / "scene-64x36-padded.desc").string());
	auto tight = load((fixtures / "scene-64x36-tight.bgr24").string(), (fixtures / "scene-64x36-tight.desc").string());
	CHECK(padded.frame && padded.frame->description.stride == 200 && padded.frame->bytes.size() == 36u * 200u);
	CHECK(tight.frame && tight.frame->description.range == "limited" && tight.frame->bytes.size() == 35u * 192u + 192u);
	CHECK(padded.frame && tight.frame && padded.frame->sha256 != tight.frame->sha256);
	// Orientation: the first bytes are the top-left marker (white), not padding
	CHECK(padded.frame && padded.frame->bytes[0] == 255 && padded.frame->bytes[1] == 255 && padded.frame->bytes[2] == 255);
	CHECK(padded.frame && padded.frame->bytes[64 * 3] == 0xee);   // first padding byte of row 0

	// JSON: a synthetic run says so, with every file field null; a file run names its input
	Run synthetic;
	synthetic.width = 2; synthetic.height = 2; synthetic.stride = 6;
	Case c; c.range = "full"; c.output = "full"; c.pattern = "random"; c.samplesNs = { 5 };
	synthetic.cases = { c };
	CHECK(contains(json(synthetic, false), "\"input\":{\"kind\":\"synthetic\",\"name\":null,\"sha256\":null,\"format\":null,\"declared_range\":null,\"orientation\":null,\"payload_bytes\":null,\"source\":null}"));
	CHECK(!contains(text(synthetic, 1, 1), "input file"));
	if (tight.frame)
	{
		Run fromFile = synthetic;
		fromFile.input.kind = "file";
		fromFile.input.name = tight.frame->name;
		fromFile.input.sha256 = tight.frame->sha256;
		fromFile.input.declaredRange = std::string("limited");
		fromFile.input.payloadBytes = 6912;
		CHECK(contains(json(fromFile, false), "\"kind\":\"file\",\"name\":\"scene-64x36-tight.bgr24\",\"sha256\":\"" + tight.frame->sha256 + "\""));
		CHECK(contains(json(fromFile, false), "\"payload_bytes\":6912"));
		CHECK(contains(text(fromFile, 1, 1), "input file: scene-64x36-tight.bgr24 sha256 " + tight.frame->sha256 + " (declared range limited; saved frame, decoder timing only, not a live-capture measurement)\n"));
	}
}

static fs::path fakeSys(const fs::path& root, bool full)
{
	fs::remove_all(root);
	const fs::path cpu = root / "sys/devices/system/cpu/cpu4";
	write(cpu / "regs/identification/midr_el1", "0x00000000414fd0b0\n");
	write(root / "proc/loadavg", "0.42 0.30 0.25 1/234 5678\n");
	write(root / "sys/class/thermal/thermal_zone0/type", "soc-thermal\n");
	write(root / "sys/class/thermal/thermal_zone0/temp", "41500\n");
	write(root / "sys/class/thermal/thermal_zone1/type", "bigcore0-thermal\n");
	write(root / "sys/class/thermal/thermal_zone1/temp", "41x\n");   // malformed value: missing, not 41 or 0
	if (full)
	{
		write(cpu / "cpufreq/scaling_governor", "performance\n");
		write(cpu / "cpufreq/scaling_cur_freq", "0\n");   // an observed zero stays zero
		write(cpu / "cpufreq/scaling_min_freq", "408000\n");
		write(cpu / "cpufreq/scaling_max_freq", "2400000\n");
		write(root / "proc/cpuinfo", "processor\t: 4\nBogoMIPS\t: 48.00\n");
	}
	return root;
}

static void metadata(const fs::path& tmp)
{
	CHECK(coreName("0x00000000414fd0b0") == "Cortex-A76");
	CHECK(coreName("0x00000000412fd050") == "Cortex-A55");
	CHECK(!coreName("0x00000000410fd4f0"));   // unknown part
	CHECK(!coreName("0x00000000510fd0b0"));   // not ARM Ltd.
	CHECK(!coreName("zz"));

	{
		const SysRoot sys{ fakeSys(tmp / "full", true).string() };
		const auto m = collect(sys, std::vector<int>{ 4 }, 4);
		CHECK(m.pinnedCpu == 4 && m.startCpu == 4 && m.core == "Cortex-A76" && m.midr == "0x00000000414fd0b0");
		CHECK(m.governor == "performance" && m.minFreqKhz == 408000 && m.maxFreqKhz == 2400000);
		CHECK(m.before.curFreqKhz && *m.before.curFreqKhz == 0);
		CHECK(m.before.loadavg && (*m.before.loadavg)[0] == 0.42);
		CHECK(m.before.thermal.size() == 2 && m.before.thermal[0].milliC == 41500 && !m.before.thermal[1].milliC);
		CHECK(m.before.thermal[1].type == "bigcore0-thermal");
		CHECK(!m.modelName);   // ARM cpuinfo has no "model name"
	}
	{
		const SysRoot sys{ fakeSys(tmp / "sparse", false).string() };
		const auto m = collect(sys, std::vector<int>{ 4 }, 4);
		CHECK(!m.governor && !m.minFreqKhz && !m.maxFreqKhz && !m.before.curFreqKhz);
		// Not pinned: per-core sysfs would describe a CPU we may not run on.
		const auto u = collect(sys, std::vector<int>{ 0, 1, 2, 3, 4 }, 2);
		CHECK(!u.pinnedCpu && u.startCpu == 2 && !u.midr && !u.core && !u.governor && !u.before.curFreqKhz);
		const auto none = collect(SysRoot{ (tmp / "nothing").string() }, std::nullopt, std::nullopt);
		CHECK(!none.affinity && !none.pinnedCpu && !none.before.loadavg && none.before.thermal.empty());
	}
}

static Run sampleRun(const fs::path& tmp, const std::string& label, int cpu, int64_t scale)
{
	Run run;
	run.width = 1920; run.height = 1080; run.stride = 5760; run.lutBytes = 50331648; run.lutFill = "constant 0x80"; run.seed = 12345;
	const SysRoot sys{ fakeSys(tmp / ("sys-" + label), true).string() };
	run.meta = collect(sys, std::vector<int>{ cpu }, cpu);
	if (label == "A55") run.meta.midr = "0x00000000412fd050", run.meta.core = "Cortex-A55";
	run.meta.commit = "0123456789abcdef0123456789abcdef01234567";
	run.meta.label = label;
	run.meta.after = run.meta.before;
	run.meta.after.curFreqKhz = std::nullopt;   // e.g. sysfs vanished: must stay null
	for (std::string pattern : { "random", "gradient" })
		for (bool limited : { false, true })
			for (bool quarter : { false, true })
				for (bool lut : { false, true })
				{
					Case c;
					c.range = limited ? "limited" : "full"; c.output = quarter ? "quarter" : "full"; c.lut = lut; c.pattern = pattern;
					c.outputWidth = quarter ? 960 : 1920; c.outputHeight = quarter ? 540 : 1080; c.warmup = 3;
					// Synthetic, deterministic timings: not measurements.
					const int64_t base = (lut ? (pattern == "random" ? 50000000 : 3000000) : 600000) * (limited ? 2 : 1) / (quarter ? 4 : 1) * scale;
					c.firstNs = base * 3;
					for (int i = 0; i < 11; ++i) c.samplesNs.push_back(base + (i * 37 % 11) * (base / 100));
					if (lut) c.profile = AccessProfile{ uint64_t(c.outputWidth) * c.outputHeight, 1000, 900, 50 };
					run.cases.push_back(c);
				}
	return run;
}

static void writers(const fs::path& tmp)
{
	// Text: the original header and table layout, byte for byte.
	Run run;
	run.width = 1920; run.height = 1080; run.stride = 5760;
	Case a; a.range = "full"; a.output = "full"; a.lut = false; a.pattern = "random"; a.samplesNs = { 618000, 684000, 600000 };
	Case b = a; b.lut = true; b.samplesNs = { 52894000 };
	run.cases = { a, b };
	const std::string expected =
		"BGR24 decode 1920x1080 stride 5760: 3 timed runs per case after 10 warm-up (ms)\n"
		"range    output   lut      median        p95        min\n"
		"full     full     no        0.618      0.684      0.600\n"
		"full     full     yes      52.894     52.894     52.894\n";
	CHECK(text(run, 3, 10) == expected);
	Case g = a; g.pattern = "gradient";
	run.cases.push_back(g);
	const auto t = text(run, 3, 10);
	CHECK(t.rfind(expected, 0) == 0 && contains(t, "input pattern: gradient\nrange    output"));
	CHECK(!contains(text(Run{ 1, 1, 3, 0, "", 0, {}, { g }, {} }, 1, 1), "input pattern: random"));

	// JSON: missing is null, observed zero is 0, raw samples only on request.
	const auto s = sampleRun(tmp, "A76", 4, 1);
	const auto j = json(s, false), jr = json(s, true);
	CHECK(contains(j, "\"schema\": \"hyperhdr.bgr24-bench/1\""));
	CHECK(contains(j, "\"before\":{\"cur_freq_khz\":0,") && contains(j, "\"after\":{\"cur_freq_khz\":null,"));
	CHECK(contains(j, "{\"zone\":\"thermal_zone1\",\"type\":\"bigcore0-thermal\",\"milli_c\":null}"));
	CHECK(contains(j, "\"model_name\":null") && contains(j, "\"core\":\"Cortex-A76\""));
	CHECK(!contains(j, "raw_ns") && contains(jr, "\"raw_ns\":["));
	CHECK(contains(j, "\"scenario\":\"limited-quarter-lut-gradient\""));
	CHECK(contains(j, "\"lut_access\":null") && contains(j, "\"lut_access\":{\"lookups\":518400,"));
	CHECK(jsonString("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");

	// CSV: one header, a row per case, same column count everywhere, NA for missing.
	const auto c = csv(s);
	std::istringstream in(c);
	std::string line;
	std::getline(in, line);
	const auto columns = std::count(line.begin(), line.end(), ',');
	int rows = 0;
	while (std::getline(in, line))
	{
		++rows;
		CHECK(std::count(line.begin(), line.end(), ',') == columns);
	}
	CHECK(rows == 16);
	CHECK(contains(c, ",A76,Cortex-A76,4,performance,0,NA,41500,41500,full-full-nolut-random,") && contains(c, ",NA\n"));
	// Missing string metadata is NA too (no commit, label, core or governor).
	Run bare = s;
	bare.meta = Metadata{};
	CHECK(contains(csv(bare), "hyperhdr.bgr24-bench/1,NA,NA,NA,NA,NA,NA,NA,NA,NA,full-full-nolut-random,"));
	CHECK(csvField(std::string("a,b")) == "\"a,b\"" && csvField(std::string("q\"")) == "\"q\"\"\"");
}

int main(int argc, char** argv)
{
	const fs::path tmp = fs::temp_directory_path() / ("bench-report-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	statistics();
	accessProfiles();
	inputs();
	options();
	metadata(tmp);
	writers(tmp);
	rawFrames(tmp);
	if (argc == 3 && std::string(argv[1]) == "--emit-sample")
	{
		const fs::path out = argv[2];
		fs::create_directories(out);
		// A synthetic A76 and A55 pair (the A55 run 2.5x slower); NOT measurements.
		std::ofstream(out / "bgr24-bench-a76.json") << json(sampleRun(tmp, "A76", 4, 1), true);
		const auto a55 = sampleRun(tmp, "A55", 0, 1);
		auto slow = a55;
		for (auto& cs : slow.cases) for (auto& v : cs.samplesNs) v = v * 5 / 2;
		std::ofstream(out / "bgr24-bench-a55.json") << json(slow, true);
		std::ofstream(out / "bgr24-bench-a76.csv") << csv(sampleRun(tmp, "A76", 4, 1));
	}
	fs::remove_all(tmp);
	if (failures) { std::cerr << failures << " check(s) failed\n"; return 1; }
	std::cout << "PASS bench report: statistics, LUT access profile, inputs, options, metadata, text/JSON/CSV\n";
	return 0;
}
