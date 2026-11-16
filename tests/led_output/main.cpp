// LED output counters and DDP packetization (issue #9), without a device or
// network. The model below mirrors LedDevice's three update paths:
//
//   smoothing        refresh timer -> smoothing -> presented -> rewriteLEDs
//   refresh only     presented stores colours; refresh timer -> rewriteLEDs
//   no refresh       presented -> queued rewriteLEDs (several may coalesce)
//
// and a DDP driver whose datagrams go through a scripted socket, counted the way
// ProviderUdp::writeBytes counts them.
#include <led-drivers/LedOutputStats.h>
#include <led-drivers/net/DdpPacketizer.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;
#define REQUIRE(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static bool contains(const std::string& s, const std::string& part)
{
	if (s.find(part) != std::string::npos)
		return true;
	std::fprintf(stderr, "  missing \"%s\" in: %s\n", part.c_str(), s.c_str());
	return false;
}

namespace
{
using Datagram = std::vector<uint8_t>;

std::vector<uint8_t> colours(size_t bytes, uint8_t seed)
{
	std::vector<uint8_t> v(bytes);
	for (size_t i = 0; i < bytes; ++i)
		v[i] = static_cast<uint8_t>(seed + i * 7);
	return v;
}

std::vector<Datagram> packetize(const std::vector<uint8_t>& data, bool rgbw, uint8_t seq)
{
	std::vector<Datagram> out;
	std::vector<uint8_t> buffer;
	Ddp::sendFrame(data.data(), data.size(), rgbw, seq, buffer,
		[&](const uint8_t* d, size_t n) { out.emplace_back(d, d + n); });
	return out;
}

uint32_t be32(const Datagram& d, size_t at) { return (uint32_t(d[at]) << 24) | (uint32_t(d[at + 1]) << 16) | (uint32_t(d[at + 2]) << 8) | d[at + 3]; }
uint16_t be16(const Datagram& d, size_t at) { return static_cast<uint16_t>((d[at] << 8) | d[at + 1]); }

// A UDP device: DDP driver + socket. fail(n) makes the n-th datagram (0-based,
// counted over the device's lifetime) be rejected by the socket.
struct DdpDevice
{
	explicit DdpDevice(LedOutput::Stats& counters) : stats(counters) {}

	LedOutput::Stats& stats;
	bool rgbw = false;
	size_t leds = 300;
	std::set<uint64_t> rejected;
	uint64_t datagramIndex = 0;
	uint8_t sequence = 0;
	std::vector<uint8_t> buffer;

	int write()
	{
		const auto data = colours(leds * (rgbw ? 4 : 3), sequence);
		sequence = static_cast<uint8_t>((sequence % 0x0F) + 1);
		Ddp::sendFrame(data.data(), data.size(), rgbw, sequence, buffer,
			[&](const uint8_t*, size_t n) { stats.packet(n, rejected.count(datagramIndex++) == 0); });
		return static_cast<int>(leds);   // like DriverNetDDP: success even if a packet failed
	}

	// rewriteLEDs
	int update()
	{
		stats.beginUpdate();
		const int rc = write();
		stats.endUpdate(rc);
		return rc;
	}

	// switchOff -> writeBlack: datagrams outside an update
	void black(int times)
	{
		for (int i = 0; i < times; ++i)
			write();
	}
};
}

static void testDdpSinglePacketLayout()
{
	const auto data = colours(300 * 3, 5);
	const auto p = packetize(data, false, 7);
	REQUIRE(p.size() == 1);
	REQUIRE(Ddp::packetCount(data.size(), false) == 1);
	REQUIRE(p[0].size() == 910);
	REQUIRE(p[0][0] == 0x41);   // version 1 + PUSH: the only packet is the last
	REQUIRE(p[0][1] == 7);
	REQUIRE(p[0][2] == 0x0B);
	REQUIRE(p[0][3] == 0x01);
	REQUIRE(be32(p[0], 4) == 0);
	REQUIRE(be16(p[0], 8) == 900);
	REQUIRE(Datagram(p[0].begin() + 10, p[0].end()) == data);
	std::puts("ok   DDP single packet: 300 RGB LEDs -> one 910-byte datagram with PUSH");
}

static void testDdpMultiPacketLayout()
{
	// 1000 RGB LEDs = 3000 bytes -> 1440 + 1440 + 120
	const auto data = colours(1000 * 3, 1);
	const auto p = packetize(data, false, 3);
	REQUIRE(p.size() == 3);
	REQUIRE(Ddp::packetCount(data.size(), false) == 3);
	const uint32_t offsets[] = { 0, 1440, 2880 };
	const uint16_t lengths[] = { 1440, 1440, 120 };
	Datagram joined;
	for (size_t i = 0; i < p.size(); ++i)
	{
		REQUIRE(p[i][0] == (i + 1 == p.size() ? 0x41 : 0x40));   // PUSH only on the last
		REQUIRE(p[i][1] == 3);                                   // one sequence per frame
		REQUIRE(be32(p[i], 4) == offsets[i]);
		REQUIRE(be16(p[i], 8) == lengths[i]);
		REQUIRE(p[i].size() == size_t(lengths[i]) + Ddp::headerSize);
		joined.insert(joined.end(), p[i].begin() + 10, p[i].end());
	}
	REQUIRE(joined == data);

	// RGBW: 360 LEDs per packet, type 0x1B. 500 LEDs = 2000 bytes -> 1440 + 560
	const auto w = packetize(colours(500 * 4, 9), true, 15);
	REQUIRE(w.size() == 2);
	REQUIRE(w[0][2] == 0x1B && w[1][2] == 0x1B);
	REQUIRE(be16(w[0], 8) == 1440 && be16(w[1], 8) == 560);
	REQUIRE(be32(w[1], 4) == 1440);

	// Boundaries: exactly full packets don't add an empty one; zero LEDs send nothing
	REQUIRE(packetize(colours(480 * 3, 0), false, 1).size() == 1);
	REQUIRE(packetize(colours(481 * 3, 0), false, 1).size() == 2);
	REQUIRE(packetize(colours(960 * 3, 0), false, 1).size() == 2);
	REQUIRE(packetize(colours(960 * 3, 0), false, 1)[1][0] == 0x41);
	REQUIRE(packetize({}, false, 1).empty());
	REQUIRE(Ddp::packetCount(0, false) == 0);
	std::puts("ok   DDP multi-packet: offsets, lengths, PUSH on last, RGBW limits, boundaries");
}

// Upstream DriverNetDDP's loop before it moved to DdpPacketizer.h, kept verbatim
// except qToBigEndian, written out. The packetizer must produce identical bytes.
static std::vector<Datagram> upstreamLoop(const std::vector<uint8_t>& data, bool isRgbw, uint8_t sequenceNum)
{
	std::vector<Datagram> sent;
	std::vector<uint8_t> _ddpFrame;
	const size_t colorSize = (isRgbw) ? 4 : 3;
	const size_t maxLedsInFrame = (isRgbw) ? 360 : 480;
	const uint8_t* start = data.data();
	const uint8_t* end = start + (data.size() / colorSize) * colorSize;
	uint32_t colorOffset = 0;
	constexpr size_t ddpHeaderOverhead = 10;
	const size_t maxColorsInUdpFrame = maxLedsInFrame * colorSize;
	size_t realRgbSize = std::min<size_t>(end - start, maxColorsInUdpFrame);
	_ddpFrame.reserve(realRgbSize + ddpHeaderOverhead);
	while (start < end)
	{
		realRgbSize = std::min<size_t>(end - start, maxColorsInUdpFrame);
		_ddpFrame.resize(realRgbSize + ddpHeaderOverhead, 0);
		bool isLast = (start + realRgbSize >= end);
		_ddpFrame[0] = (isLast) ? 0x41 : 0x40;
		_ddpFrame[1] = sequenceNum;
		_ddpFrame[2] = (isRgbw) ? 0x1B : 0x0B;
		_ddpFrame[3] = 0x01;
		for (int i = 0; i < 4; ++i) _ddpFrame[4 + i] = static_cast<uint8_t>(colorOffset >> (24 - 8 * i));
		const uint16_t len = static_cast<uint16_t>(realRgbSize);
		_ddpFrame[8] = static_cast<uint8_t>(len >> 8);
		_ddpFrame[9] = static_cast<uint8_t>(len);
		memcpy(_ddpFrame.data() + ddpHeaderOverhead, start, realRgbSize);
		start += realRgbSize;
		colorOffset += static_cast<uint32_t>(realRgbSize);
		sent.push_back(_ddpFrame);
	}
	return sent;
}

static void testDdpMatchesUpstreamLoop()
{
	int compared = 0;
	for (bool rgbw : { false, true })
		for (size_t leds : { 0, 1, 2, 100, 359, 360, 361, 479, 480, 481, 719, 720, 721, 959, 960, 961, 1500, 4000 })
		{
			const auto data = colours(leds * (rgbw ? 4 : 3), static_cast<uint8_t>(leds));
			const uint8_t seq = static_cast<uint8_t>(1 + leds % 15);
			REQUIRE(packetize(data, rgbw, seq) == upstreamLoop(data, rgbw, seq));
			++compared;
		}
	REQUIRE(compared == 36);
	std::puts("ok   DDP packetizer is byte-identical to the upstream loop (36 sizes, RGB and RGBW)");
}

static void testSinglePacketOutput()
{
	LedOutput::Stats s;
	s.countPackets();
	DdpDevice dev(s);
	s.begin(0);
	for (int i = 0; i < 60; ++i)
	{
		s.presented(true);
		dev.update();
	}
	const auto [seconds, d] = s.take(1000);
	REQUIRE(seconds == 1.0);
	REQUIRE(d.presented == 60 && d.updates == 60 && d.repeats == 0);
	REQUIRE(d.packets == 60 && d.maxPacketsPerUpdate == 1);
	REQUIRE(d.bytes == 60u * 910u);
	REQUIRE(d.failed == 0 && d.packetFailures == 0);
	REQUIRE(LedOutput::reportLevel(d) == LedOutput::ReportLevel::Debug);
	const auto line = LedOutput::Stats::format(d, seconds, "ddp");
	REQUIRE(contains(line, "LED output stats (ddp): 1.0 s"));
	REQUIRE(contains(line, "updates 60 (60.00/s)"));
	REQUIRE(contains(line, "packets 60 (1.00/update, max 1)"));
	REQUIRE(contains(line, "54600 bytes accepted"));
	REQUIRE(s.consistent());
	std::puts("ok   single-packet output: one update = one group = one packet");
}

static void testMultiPacketIsOneGroup()
{
	LedOutput::Stats s;
	s.countPackets();
	DdpDevice dev(s);
	dev.leds = 1000;
	s.begin(0);
	for (int i = 0; i < 30; ++i)
	{
		s.presented(true);
		dev.update();
	}
	const auto [seconds, d] = s.take(1000);
	REQUIRE(d.updates == 30);           // the output rate counts groups...
	REQUIRE(d.packets == 90);           // ...not packets
	REQUIRE(d.maxPacketsPerUpdate == 3);
	const auto line = LedOutput::Stats::format(d, seconds);
	REQUIRE(contains(line, "updates 30 (30.00/s)"));
	REQUIRE(contains(line, "packets 90 (3.00/update, max 3)"));
	REQUIRE(line.find("90.00/s") == std::string::npos);
	REQUIRE(s.consistent());
	std::puts("ok   multi-packet output: 3 datagrams per update counted as one output group");
}

static void testWriteFailures()
{
	LedOutput::Stats s;
	s.countPackets();
	DdpDevice dev(s);
	dev.leds = 1000;   // 3 datagrams per update
	dev.rejected = { 4 };   // the second datagram of the second update
	s.begin(0);
	for (int i = 0; i < 3; ++i)
	{
		s.presented(true);
		REQUIRE(dev.update() == 1000);   // the driver itself reports success
	}
	auto [seconds, d] = s.take(1000);
	REQUIRE(d.updates == 3);
	REQUIRE(d.failed == 1);
	REQUIRE(d.packets == 9 && d.packetFailures == 1);
	REQUIRE(d.bytes == 2u * (1450u + 1450u + 130u) + 1450u + 130u);   // rejected bytes excluded
	REQUIRE(LedOutput::reportLevel(d) == LedOutput::ReportLevel::Info);
	REQUIRE(contains(LedOutput::Stats::format(d, seconds), "failed 1"));
	REQUIRE(contains(LedOutput::Stats::format(d, seconds), "rejected by socket 1"));

	// A driver that doesn't use ProviderUdp: only its return value is known
	LedOutput::Stats serial;
	serial.begin(0);
	serial.presented(true);
	serial.beginUpdate();
	serial.endUpdate(-1);
	serial.presented(true);
	serial.beginUpdate();
	serial.endUpdate(42);
	std::tie(seconds, d) = serial.take(2000);
	REQUIRE(d.updates == 2 && d.failed == 1 && d.packets == 0);
	const auto line = LedOutput::Stats::format(d, seconds);
	REQUIRE(contains(line, "packets n/a"));
	REQUIRE(line.find("packets 0") == std::string::npos);
	REQUIRE(serial.consistent());
	std::puts("ok   write failures: a rejected packet fails its update even when the driver returns success");
}

static void testZeroOutputIntervals()
{
	LedOutput::Stats s;
	s.countPackets();
	s.begin(0);
	auto [seconds, d] = s.take(60000);
	REQUIRE(!d.hasActivity());
	REQUIRE(LedOutput::reportLevel(d) == LedOutput::ReportLevel::Debug);
	REQUIRE(contains(LedOutput::Stats::format(d, seconds), "60.0 s, no LED output (0 presented, 0 updates)"));

	// Presented while the device is off: counted, but not output
	s.presented(false);
	s.presented(false);
	std::tie(seconds, d) = s.take(120000);
	REQUIRE(d.presented == 2 && d.inactive == 2 && d.updates == 0);
	REQUIRE(contains(LedOutput::Stats::format(d, seconds), "presented 2 (0.03/s), 2 while inactive; updates 0 (0.00/s)"));

	// No elapsed time: rates are unknown, not 0
	s.presented(true);
	s.beginUpdate();
	s.endUpdate(1);
	std::tie(seconds, d) = s.take(120000);
	REQUIRE(seconds == 0.0);
	REQUIRE(contains(LedOutput::Stats::format(d, seconds), "updates 1 (n/a/s)"));
	REQUIRE(s.consistent());
	std::puts("ok   zero-output intervals: reported as no output; unknown rates as n/a");
}

static void testSmoothingGeneratedUpdates()
{
	LedOutput::Stats s;
	s.countPackets();
	DdpDevice dev(s);
	s.begin(0);

	// Smoothing: one captured frame, then 60 refresh ticks each producing
	// interpolated colours. The LED output rate follows the clock, not captures.
	const int capturedFrames = 1;
	for (int tick = 0; tick < 60; ++tick)
	{
		s.presented(true);   // smoothing emits on every tick
		dev.update();        // same thread: written immediately
	}
	auto [seconds, d] = s.take(1000);
	REQUIRE(d.presented == 60 && d.updates == 60 && d.repeats == 0);
	REQUIRE(d.updates != static_cast<uint64_t>(capturedFrames));

	// Refresh timer without smoothing: one new colour set, resent on each tick
	s.presented(true);
	for (int tick = 0; tick < 10; ++tick)
		dev.update();
	std::tie(seconds, d) = s.take(2000);
	REQUIRE(d.presented == 1 && d.updates == 10 && d.repeats == 9 && d.freshUpdates() == 1);
	REQUIRE(contains(LedOutput::Stats::format(d, seconds), "updates 10 (10.00/s) (new colours 1, repeats 9)"));

	// No refresh timer: updates are queued; three arrive before the write runs
	s.presented(true);
	s.presented(true);
	s.presented(true);
	dev.update();
	std::tie(seconds, d) = s.take(3000);
	REQUIRE(d.presented == 3 && d.superseded == 2 && d.updates == 1 && d.repeats == 0);
	REQUIRE(s.consistent());
	std::puts("ok   smoothing and refresh: updates follow the LED clock; repeats and coalescing are separated");
}

static void testResetAndRestart()
{
	LedOutput::Stats s;
	s.countPackets();
	DdpDevice dev(s);
	REQUIRE(!s.running());

	s.begin(1000);
	dev.leds = 1000;
	s.presented(true);
	dev.update();
	auto [seconds, d] = s.take(61000);
	REQUIRE(d.updates == 1 && d.maxPacketsPerUpdate == 3);

	// The next interval starts from zero, including the per-interval maximum
	dev.leds = 300;
	s.presented(true);
	dev.update();
	std::tie(seconds, d) = s.take(121000);
	REQUIRE(d.updates == 1 && d.packets == 1 && d.maxPacketsPerUpdate == 1);

	// Disable: colours presented but not yet written are superseded, black frames
	// are datagrams outside an update, and the interval ends
	s.presented(true);
	REQUIRE(s.pendingFresh());
	s.restart();
	REQUIRE(!s.pendingFresh());
	dev.black(2);
	std::tie(seconds, d) = s.take(130000);
	REQUIRE(d.superseded == 1 && d.updates == 0);
	REQUIRE(d.otherPackets == 2 && d.packets == 0);
	REQUIRE(contains(LedOutput::Stats::format(d, seconds, "device disabled"), "LED output stats (device disabled): 9.0 s"));
	REQUIRE(contains(LedOutput::Stats::format(d, seconds), "outside updates 2 packets (0 rejected)"));
	s.end();
	REQUIRE(!s.running());

	// Re-enable: a new interval; lifetime counters stay consistent
	s.begin(200000);
	s.presented(true);
	dev.update();
	std::tie(seconds, d) = s.take(260000);
	REQUIRE(seconds == 60.0);
	REQUIRE(d.updates == 1 && d.superseded == 0 && d.otherPackets == 0);
	REQUIRE(s.consistent());
	std::puts("ok   reset and restart: per-interval deltas, flush on disable, clean re-enable");
}

static void testRandomSequencesStayConsistent()
{
	std::mt19937 rng(20261116);
	LedOutput::Stats s;
	s.countPackets();
	DdpDevice dev(s);
	s.begin(0);
	int64_t now = 0;
	for (int step = 0; step < 40000; ++step)
	{
		switch (rng() % 6)
		{
			case 0: case 1: s.presented(true); break;
			case 2: s.presented(false); break;
			case 3: dev.leds = 1 + rng() % 1500; dev.update(); break;
			case 4: if (rng() % 50 == 0) { s.restart(); dev.black(1); } break;
			case 5: now += rng() % 2000; if (rng() % 30 == 0) s.take(now); break;
		}
		if (!s.consistent())
		{
			REQUIRE(s.consistent());
			return;
		}
	}
	const auto total = s.snapshot();
	REQUIRE(total.presented > 0 && total.updates > 0);
	std::puts("ok   40000 random steps: presented == inactive + superseded + new-colour updates + pending");
}

// The wled driver (DriverNetWled::writeFiniteColors): WLED realtime UDP, not DDP.
// Up to 490 LEDs one DRGB datagram (2-byte header); above, DNRGB datagrams of up
// to 489 LEDs (4-byte header). A changed LED count is adopted and nothing is
// sent (returns 0). Every datagram goes through ProviderUdp::writeBytes.
struct WledDevice
{
	explicit WledDevice(LedOutput::Stats& counters) : stats(counters) {}
	LedOutput::Stats& stats;
	size_t configured = 300;
	std::set<uint64_t> rejected;
	uint64_t datagramIndex = 0;

	int write(size_t leds)
	{
		if (leds != configured) { configured = leds; return 0; }
		auto send = [&](size_t bytes) { const bool ok = rejected.count(datagramIndex++) == 0; stats.packet(bytes, ok); return ok ? 0 : -1; };
		if (leds <= 490)
			return send(2 + leds * 3);
		for (size_t done = 0; done < leds; done += 489)
			send(4 + std::min<size_t>(489, leds - done) * 3);
		return int(leds * 3);
	}
	int update(size_t leds)
	{
		stats.beginUpdate();
		const int rc = write(leds);
		stats.endUpdate(rc);
		return rc;
	}
};

static void testWledDriverAccounting()
{
	LedOutput::Stats s;
	s.countPackets();
	WledDevice dev(s);
	s.begin(0);
	s.presented(true);
	dev.update(300);                 // DRGB: one datagram
	s.presented(true);
	dev.configured = 1000;
	dev.update(1000);                // DNRGB: 489 + 489 + 22 LEDs
	auto [seconds, d] = s.take(1000);
	REQUIRE(d.updates == 2 && d.packets == 4 && d.maxPacketsPerUpdate == 3);
	REQUIRE(d.bytes == (2u + 900u) + (4u + 1467u) * 2 + (4u + 66u));
	REQUIRE(d.failed == 0 && d.unsent == 0);

	// Rejected DNRGB datagram: the driver still returns success, the update fails
	dev.rejected = { dev.datagramIndex + 1 };
	s.presented(true);
	REQUIRE(dev.update(1000) > 0);
	// Rejected DRGB datagram: the driver returns -1 itself
	dev.configured = 300;
	dev.rejected = { dev.datagramIndex };
	s.presented(true);
	REQUIRE(dev.update(300) == -1);
	std::tie(seconds, d) = s.take(2000);
	REQUIRE(d.updates == 2 && d.failed == 2 && d.packetFailures == 2 && d.unsent == 0);

	// LED count change: the driver returns 0 and sends nothing. That is not a
	// transmitted update, and not a failure either.
	s.presented(true);
	REQUIRE(dev.update(301) == 0);
	std::tie(seconds, d) = s.take(3000);
	REQUIRE(d.updates == 1 && d.packets == 0 && d.failed == 0 && d.unsent == 1);
	REQUIRE(LedOutput::reportLevel(d) == LedOutput::ReportLevel::Info);
	REQUIRE(contains(LedOutput::Stats::format(d, seconds), "failed 0, 1 sent no packet; packets 0"));
	REQUIRE(s.consistent());

	// A driver without packet counting (serial, SPI, ...) never reports "sent no packet"
	LedOutput::Stats serial;
	serial.begin(0);
	serial.presented(true);
	serial.beginUpdate();
	serial.endUpdate(0);
	std::tie(seconds, d) = serial.take(1000);
	REQUIRE(d.unsent == 0 && contains(LedOutput::Stats::format(d, seconds), "packets n/a"));
	std::puts("ok   wled driver: DRGB/DNRGB datagrams counted, failures through either return path, unsent updates separated");
}

// The packet layouts and ports modelled in these tests are the drivers' own.
static void testDriverConstantsUnchanged()
{
	auto read = [](const char* rel) {
		std::ifstream f(std::string(HYPERHDR_SOURCE_DIR) + "/" + rel);
		return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	};
	const std::string wled = read("sources/led-drivers/net/DriverNetWled.cpp");
	const std::string ddp = read("sources/led-drivers/net/DriverNetDDP.cpp");
	REQUIRE(contains(wled, "_warlsStreamPort(21324)"));               // WLED realtime UDP default port
	REQUIRE(contains(wled, "ledValues.size() <= 490"));                // DRGB up to 490 LEDs
	REQUIRE(contains(wled, "489 * sizeof(ColorRgb)"));                 // DNRGB 489 LEDs per datagram
	REQUIRE(contains(wled, "return writeBytes(wledSize, wledData.data());"));
	REQUIRE(contains(ddp, "DDP_DEFAULT_PORT = 4048"));                 // DDP default port
	REQUIRE(contains(ddp, "Ddp::sendFrame("));
	std::puts("ok   driver constants: wled 21324, DRGB <= 490, DNRGB 489/datagram; ddp 4048 through Ddp::sendFrame");
}

static void testReEnableAfterErrorKeepsTheInterval()
{
	LedOutput::Stats s;
	s.countPackets();
	DdpDevice dev(s);
	REQUIRE(!s.start(0));                  // first enable: nothing pending
	for (int i = 0; i < 5; ++i)
	{
		s.presented(true);
		dev.update();
	}
	dev.rejected = { dev.datagramIndex };
	s.presented(true);
	dev.update();                          // the failure that put the device in error
	s.presented(false);                    // presented while in error
	// setInError doesn't disable, so the interval is still running at re-enable
	const auto pending = s.start(30000);
	REQUIRE(pending.has_value());
	if (pending)
	{
		REQUIRE(pending->first == 30.0);
		REQUIRE(pending->second.updates == 6 && pending->second.failed == 1 && pending->second.inactive == 1);
		REQUIRE(contains(LedOutput::Stats::format(pending->second, pending->first, "until re-enable"), "LED output stats (until re-enable): 30.0 s"));
	}
	auto [seconds, d] = s.take(90000);
	REQUIRE(seconds == 60.0 && d.updates == 0 && d.failed == 0);   // the new interval starts clean
	s.end();
	REQUIRE(!s.start(100000));
	REQUIRE(s.consistent());
	std::puts("ok   re-enable after an error reports the running interval instead of discarding it");
}

static void testIndependentDevices()
{
	LedOutput::Stats a, b;
	a.countPackets();
	b.countPackets();
	DdpDevice ddp(a);
	WledDevice wled(b);
	ddp.leds = 1000;
	wled.configured = 600;
	a.begin(0);
	b.begin(0);
	for (int i = 0; i < 10; ++i)
	{
		a.presented(true);
		b.presented(true);
		ddp.update();
		if (i % 2 == 0)
			wled.update(600);
	}
	b.presented(false);
	const auto ra = a.take(1000).second, rb = b.take(1000).second;
	REQUIRE(ra.presented == 10 && ra.updates == 10 && ra.packets == 30 && ra.maxPacketsPerUpdate == 3 && ra.inactive == 0);
	REQUIRE(rb.presented == 11 && rb.updates == 5 && rb.packets == 10 && rb.maxPacketsPerUpdate == 2 && rb.inactive == 1 && rb.superseded == 4 && b.pendingFresh());
	REQUIRE(a.consistent() && b.consistent());
	std::puts("ok   two devices: independent counters, rates and maxima");
}

static void testPresentedFromAnotherThread()
{
	LedOutput::Stats s;
	s.countPackets();
	DdpDevice dev(s);
	std::thread producer([&] { for (int i = 0; i < 20000; ++i) s.presented(true); });
	for (int i = 0; i < 5000; ++i)
		dev.update();
	producer.join();
	const auto t = s.snapshot();
	REQUIRE(t.presented == 20000);
	REQUIRE(t.updates == 5000);
	REQUIRE(s.consistent());
	std::puts("ok   presented on another thread: no lost counts");
}

int main()
{
	testDdpSinglePacketLayout();
	testDdpMultiPacketLayout();
	testDdpMatchesUpstreamLoop();
	testSinglePacketOutput();
	testMultiPacketIsOneGroup();
	testWriteFailures();
	testZeroOutputIntervals();
	testSmoothingGeneratedUpdates();
	testResetAndRestart();
	testRandomSequencesStayConsistent();
	testPresentedFromAnotherThread();
	testWledDriverAccounting();
	testDriverConstantsUnchanged();
	testReEnableAfterErrorKeepsTheInterval();
	testIndependentDevices();
	if (failures)
	{
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	std::puts("all LED output tests passed");
	return 0;
}
