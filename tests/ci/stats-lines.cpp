// Prints `capture stats` and `LED output stats` messages from the real
// formatters (V4L2Stats.h, LedOutputStats.h; both Qt-free), one per line as
// "<kind>|<message>". tests/ci/stats-align-test.py wraps them in journal lines,
// so the parser in scripts/rk3588/stats-align.py is checked against the format
// HyperHDR actually logs, not a copy of it.
#include <grabber/linux/v4l2/V4L2Stats.h>
#include <led-drivers/LedOutputStats.h>
#include <cstdio>

int main()
{
	V4L2Capture::StatsSnapshot c;
	c.dequeued = 1800; c.accepted = 1740; c.decimated = 60; c.decoded = 1739; c.decodeFailed = 1;
	c.missedFrames = 2; c.p50Us = 4100; c.p95Us = 5200; c.p99Us = 6300; c.durationSamples = 1739; c.restarts = 0;
	std::printf("capture|%s\n", V4L2Capture::Stats::format(c, 60.0).c_str());

	V4L2Capture::StatsSnapshot s;                     // partial interval at a stop: 12.4 s
	s.dequeued = 372; s.accepted = 370; s.workerBusy = 2; s.decoded = 368; s.abandoned = 2; s.durationSamples = 368; s.restarts = 1;
	std::printf("capture-stopped|%s\n", V4L2Capture::Stats::format(s, 12.4, "capture stopped").c_str());

	V4L2Capture::StatsSnapshot m = c;                 // driver without sequence numbers, no decodes timed
	m.missedFrames.reset(); m.p50Us.reset(); m.p95Us.reset(); m.p99Us.reset(); m.durationSamples = 0;
	std::printf("capture-na|%s\n", V4L2Capture::Stats::format(m, 60.0).c_str());

	V4L2Capture::StatsSnapshot bad = c; bad.dequeued = 1801;   // dequeued != accepted + discarded
	std::printf("capture-mismatch|%s\n", V4L2Capture::Stats::format(bad, 60.0).c_str());

	LedOutput::Snapshot d;
	d.presented = 3600; d.updates = 3600; d.packets = 10800; d.maxPacketsPerUpdate = 3; d.bytes = 15606000; d.packetsCounted = true;
	std::printf("led|%s\n", LedOutput::Stats::format(d, 60.0).c_str());

	LedOutput::Snapshot r = d; r.repeats = 1800; r.failed = 3; r.packetFailures = 3; r.superseded = 5;
	std::printf("led-repeats|%s\n", LedOutput::Stats::format(r, 60.0).c_str());

	LedOutput::Snapshot n; n.presented = 600; n.updates = 600;   // serial/SPI: no UDP provider
	std::printf("led-nopackets|%s\n", LedOutput::Stats::format(n, 60.0).c_str());

	std::printf("led-idle|%s\n", LedOutput::Stats::format(LedOutput::Snapshot{}, 60.0).c_str());
	std::printf("led-disabled|%s\n", LedOutput::Stats::format(d, 12.4, "device disabled").c_str());
	return 0;
}
