// Hardware-free tests for the V4L2 pipeline counters (issue #9): the counting
// rules, sequence-gap logic, bounded percentile estimator and thread safety.
#include <grabber/linux/v4l2/V4L2Stats.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace V4L2Capture;

static int failures = 0;
#define REQUIRE(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

namespace
{
enum class Fate { Accept, WorkerBusy, Decimated, DriverError, Short, LutNotReady, Inactive, Undersized };

// Mirrors the call order in V4L2Grabber::read_frame / process_image: dequeue,
// sequence number, then exactly one outcome.
struct Sim
{
	Stats st;
	void frame(uint32_t seq, Fate fate = Fate::Accept)
	{
		st.dequeued();
		st.sequence(seq);
		switch (fate)
		{
			case Fate::Accept: st.accepted(); break;
			case Fate::WorkerBusy: st.workerBusy(); break;
			case Fate::Decimated: st.decimated(); break;
			case Fate::DriverError: st.driverError(); break;
			case Fate::Short: st.shortPayload(); break;
			case Fate::LutNotReady: st.lutNotReady(); break;
			case Fate::Inactive: st.workersInactive(); break;
			case Fate::Undersized: st.undersized(); break;
		}
	}
};

struct Rng
{
	uint32_t s = 2463534242u;
	uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
};

void testNormalCapture()
{
	Sim sim;
	for (uint32_t i = 0; i < 300; ++i) { sim.frame(i); sim.st.decoded(4000 + i); }   // 300 frames, as on the board
	const auto s = sim.st.snapshot(true);
	REQUIRE(s.dequeued == 300 && s.accepted == 300 && s.decoded == 300 && s.decodeFailed == 0);
	REQUIRE(s.discarded() == 0 && s.dropped() == 0 && s.consistent());
	REQUIRE(s.missedFrames && *s.missedFrames == 0);
	REQUIRE(s.p50Us && s.p95Us && s.p99Us && *s.p50Us <= *s.p95Us && *s.p95Us <= *s.p99Us);
	REQUIRE(s.durationSamples == 300);
}

void testMissingSequenceNumbers()
{
	Sim sim;
	for (uint32_t seq : {0u, 1u, 2u, 5u, 6u, 10u, 11u}) sim.frame(seq);   // gaps of 2 and 3
	auto s = sim.st.snapshot(true);
	REQUIRE(s.missedFrames && *s.missedFrames == 5);
	REQUIRE(s.dequeued == 7 && s.consistent());                           // missing frames were never dequeued
}

void testWorkerBusyAndDecimation()
{
	Sim sim;
	for (uint32_t i = 0; i < 30; ++i)
		sim.frame(i, i % 3 == 0 ? Fate::Decimated : (i % 5 == 0 ? Fate::WorkerBusy : Fate::Accept));
	const auto s = sim.st.snapshot(true);
	REQUIRE(s.decimated == 10 && s.workerBusy == 4 && s.accepted == 16);
	REQUIRE(s.discarded() == 14 && s.dropped() == 4);                      // decimation is a setting, not a loss
	REQUIRE(s.consistent());
}

void testShortFrameDriverErrorAndOtherCauses()
{
	Sim sim;
	sim.frame(0); sim.frame(1, Fate::Short); sim.frame(2, Fate::DriverError); sim.frame(3, Fate::LutNotReady);
	sim.frame(4, Fate::Inactive); sim.frame(5, Fate::Undersized); sim.frame(6);
	const auto s = sim.st.snapshot(true);
	REQUIRE(s.shortPayload == 1 && s.driverError == 1 && s.lutNotReady == 1 && s.workersInactive == 1 && s.undersized == 1);
	REQUIRE(s.dequeued == 7 && s.accepted == 2 && s.consistent());
	REQUIRE(s.missedFrames && *s.missedFrames == 0);                       // error buffers still carry a sequence number
}

void testRestartAndSequenceReset()
{
	Sim sim;
	sim.frame(100); sim.frame(101); sim.frame(102);
	sim.st.restart();                                                      // new stream: the driver counts from 0 again
	sim.frame(0); sim.frame(1); sim.frame(2);
	auto s = sim.st.snapshot(true);
	REQUIRE(s.restarts == 1 && s.sequenceResets == 0 && s.missedFrames && *s.missedFrames == 0);
	// Without telling the stats about it, a backwards step is recorded as a reset
	// (not a gap) and tracking re-anchors.
	Sim backwards;
	for (uint32_t seq : {50u, 51u, 3u, 4u, 6u}) backwards.frame(seq);   // 51 -> 3 is a reset; 4 -> 6 misses one
	s = backwards.st.snapshot(true);
	REQUIRE(s.sequenceResets == 1 && s.missedFrames && *s.missedFrames == 1);
	// A restart must not hide a real gap that happened before it.
	Sim gapThenRestart;
	for (uint32_t seq : {10u, 13u}) gapThenRestart.frame(seq);
	gapThenRestart.st.restart(); gapThenRestart.frame(0);
	s = gapThenRestart.st.snapshot(true);
	REQUIRE(s.missedFrames && *s.missedFrames == 2 && s.restarts == 1);
}

void testCounterResetKeepsSequenceTracking()
{
	Sim sim;
	sim.frame(0); sim.frame(1);
	auto a = sim.st.snapshot(true);                                        // interval boundary
	REQUIRE(a.dequeued == 2 && a.missedFrames && *a.missedFrames == 0);
	sim.frame(2);                                                          // continues with no false gap
	sim.frame(5);                                                          // a real gap right after the reset
	auto b = sim.st.snapshot(true);
	REQUIRE(b.dequeued == 2 && b.missedFrames && *b.missedFrames == 2);
	const auto c = sim.st.snapshot(true);                                  // an interval with nothing
	REQUIRE(c.dequeued == 0 && c.accepted == 0 && c.missedFrames && *c.missedFrames == 0 && !c.p50Us);
	sim.frame(6); sim.frame(7);
	const auto peek = sim.st.snapshot(false);                              // reading without reset leaves the counts
	REQUIRE(peek.dequeued == 2 && sim.st.snapshot(true).dequeued == 2);
}

void testSequenceWraparound()
{
	Sim sim;
	for (uint32_t seq : {0xfffffffdu, 0xfffffffeu, 0xffffffffu, 0u, 1u}) sim.frame(seq);
	auto s = sim.st.snapshot(true);
	REQUIRE(s.missedFrames && *s.missedFrames == 0 && s.sequenceResets == 0);
	Sim skipping;
	skipping.frame(0xfffffffeu); skipping.frame(1u);                       // skips 0xffffffff and 0 across the wrap
	s = skipping.st.snapshot(true);
	REQUIRE(s.missedFrames && *s.missedFrames == 2 && s.sequenceResets == 0);
}

void testProcessingFailureAndRecovery()
{
	Sim sim;
	for (uint32_t i = 0; i < 10; ++i)
	{
		sim.frame(i);
		if (i >= 3 && i < 6) sim.st.decodeFailed(); else sim.st.decoded(3000);
	}
	const auto s = sim.st.snapshot(true);
	REQUIRE(s.accepted == 10 && s.decoded == 7 && s.decodeFailed == 3 && s.decoded + s.decodeFailed <= s.accepted);
	REQUIRE(s.consistent() && s.durationSamples == 7);                     // failures contribute no duration sample
}

void testUnavailableIsNotZero()
{
	Sim sim;
	for (uint32_t i = 0; i < 5; ++i) sim.frame(0);                         // a driver that always reports sequence 0
	auto s = sim.st.snapshot(true);
	// No duplicates either: a number that never moves carries no information
	// (changed in the #32 hardening; it previously counted 4 here).
	REQUIRE(!s.missedFrames && s.duplicateSequences == 0);
	REQUIRE(!s.p50Us && !s.p95Us && !s.p99Us && !s.meanUs && s.durationSamples == 0);
	const auto line = Stats::format(s, 60.0);
	REQUIRE(line.find("missed by driver sequence n/a") != std::string::npos);
	REQUIRE(line.find("p50/p95/p99 n/a/n/a/n/a ms") != std::string::npos);
	REQUIRE(line.find("LED output rate n/a, DDP timing n/a") != std::string::npos);
	Sim healthy; healthy.frame(0); healthy.frame(1); healthy.st.decoded(2000);
	const auto line2 = Stats::format(healthy.st.snapshot(true), 60.0);
	REQUIRE(line2.find("missed by driver sequence 0;") != std::string::npos);   // a real zero is printed as 0
	REQUIRE(line2.find("ACCOUNTING MISMATCH") == std::string::npos);
	Stats broken; broken.dequeued();
	REQUIRE(Stats::format(broken.snapshot(true), 1.0).find("ACCOUNTING MISMATCH") != std::string::npos);
}

void testHistogramErrorBound()
{
	// Buckets cover the range in order, and the upper bound never underestimates
	// the value and overestimates by at most 12.5% (exact below 8 us).
	int previous = -1;
	for (uint64_t v = 0; v < 5000000; v += (v < 4096 ? 1 : 997))
	{
		const int idx = DurationHistogram::index(v);
		REQUIRE(idx >= previous && idx < DurationHistogram::Buckets);
		previous = idx;
		const uint64_t upper = DurationHistogram::upperBound(idx);
		REQUIRE(upper >= v);
		if (v < 8) REQUIRE(upper == v); else REQUIRE(static_cast<double>(upper) <= static_cast<double>(v) * 1.125 + 1);
	}
	REQUIRE(DurationHistogram::index(0xffffffffULL) == DurationHistogram::Buckets - 1);
	REQUIRE(DurationHistogram::index(~0ULL) == DurationHistogram::Buckets - 1);          // clamps, never overflows

	// Percentiles of a known distribution (1..1000 us) against the exact rank.
	DurationHistogram h;
	for (uint64_t v = 1; v <= 1000; ++v) h.add(v);
	const auto samples = h.drain(true);
	for (auto [p, exact] : {std::pair<double, double>{50, 500}, {95, 950}, {99, 990}, {100, 1000}})
	{
		const auto got = samples.percentile(p);
		REQUIRE(got && static_cast<double>(*got) >= exact && static_cast<double>(*got) <= exact * 1.125 + 1);
	}
	REQUIRE(samples.mean() && *samples.mean() == 500.5);
	// Rank is rounded up (nearest-rank method): with 7 samples p50 is the 4th, p99 the 7th, p1 the 1st.
	DurationHistogram seven;
	for (uint64_t v = 1; v <= 7; ++v) seven.add(v);                                      // exact buckets below 8 us
	const auto small = seven.drain(true);
	REQUIRE(small.percentile(50) && *small.percentile(50) == 4);
	REQUIRE(small.percentile(99) && *small.percentile(99) == 7);
	REQUIRE(small.percentile(1) && *small.percentile(1) == 1);
	REQUIRE(small.percentile(0) && *small.percentile(0) == 1);                           // never rank 0
	REQUIRE(h.drain(false).count == 0);                                                  // drained and reset
}

void testAccountingInvariantUnderRandomStreams()
{
	Rng rng;
	Sim sim; uint32_t seq = 0xfffffff0u;                                      // starts near the wrap
	uint64_t accepted = 0;
	for (int i = 0; i < 200000; ++i)
	{
		const auto r = rng.next();
		seq += 1 + (r % 50 == 0 ? r % 7 : 0);                                 // occasional gaps
		const Fate fate = static_cast<Fate>((r >> 8) % 8);
		sim.frame(seq, fate);
		if (fate == Fate::Accept) { ++accepted; if ((r >> 16) % 4) sim.st.decoded(r % 20000); else sim.st.decodeFailed(); }
		if (i % 997 == 0)
		{
			const auto s = sim.st.snapshot(true);
			REQUIRE(s.consistent());
			REQUIRE(s.decoded + s.decodeFailed <= s.accepted);
		}
		if (i % 20011 == 0) sim.st.restart(), seq = 0;
	}
	const auto last = sim.st.snapshot(true);
	REQUIRE(last.consistent());
}

void testConcurrentUpdatesAndSnapshots()
{
	Stats st;
	constexpr int threads = 4, per = 50000;
	std::atomic<bool> stop{false};
	std::atomic<uint64_t> snapshotTotal{0}, observedDecoded{0};
	std::thread reader([&] {
		while (!stop.load())
		{
			const auto s = st.snapshot(true);
			observedDecoded += s.decoded;
			snapshotTotal += s.durationSamples;
		}
	});
	std::vector<std::thread> writers;
	for (int t = 0; t < threads; ++t)
		writers.emplace_back([&] { for (int i = 0; i < per; ++i) st.decoded(static_cast<uint64_t>(i)); });
	for (auto& w : writers) w.join();
	stop = true; reader.join();
	const auto rest = st.snapshot(true);
	REQUIRE(observedDecoded + rest.decoded == static_cast<uint64_t>(threads) * per);        // nothing lost, nothing counted twice
	REQUIRE(snapshotTotal + rest.durationSamples == static_cast<uint64_t>(threads) * per);
}

// ---- hardening (PR #32 review) -------------------------------------------------
void testAbandonedByRestart()
{
	// Two workers hold frames when capture restarts; their late completions belong
	// to the old stream and are discarded, but must still be accounted for.
	Sim sim;
	for (uint32_t q = 1; q <= 5; ++q) sim.frame(q);
	sim.st.decoded(1000); sim.st.decoded(1200); sim.st.decodeFailed();
	sim.st.restart();                                  // two frames still in workers
	sim.st.abandoned(); sim.st.abandoned();            // their completions arrive after the restart
	const auto s = sim.st.snapshot(true);
	REQUIRE(s.consistent());
	REQUIRE(s.accepted == 5 && s.decoded + s.decodeFailed + s.abandoned == s.accepted);   // nothing unaccounted
	REQUIRE(s.abandoned == 2 && s.dropped() == 0);
	REQUIRE(Stats::format(s, 1.0).find("2 abandoned by restart") != std::string::npos);
}

void testDriverWithoutSequenceNumbers()
{
	// Some drivers leave v4l2_buffer.sequence at 0: no gaps, no duplicates, and the
	// gap count stays "unavailable" instead of 0.
	Sim sim;
	for (int i = 0; i < 100; ++i) sim.frame(0);
	auto s = sim.st.snapshot(true);
	REQUIRE(s.duplicateSequences == 0 && !s.missedFrames && s.sequenceResets == 0);
	// Once the number moves, a real repeat is a duplicate.
	sim.frame(1); sim.frame(2); sim.frame(2); sim.frame(3);
	s = sim.st.snapshot(true);
	REQUIRE(s.duplicateSequences == 1 && s.missedFrames && *s.missedFrames == 0);
}

void testReportLevels()
{
	StatsSnapshot none;
	REQUIRE(reportLevel(none) == ReportLevel::None);
	StatsSnapshot restartsOnly; restartsOnly.restarts = 4;          // failed starts during backoff: silent
	REQUIRE(reportLevel(restartsOnly) == ReportLevel::None);

	Sim clean; for (uint32_t q = 1; q <= 60; ++q) clean.frame(q, q % 2 ? Fate::Accept : Fate::Decimated);
	for (int i = 0; i < 30; ++i) clean.st.decoded(800);
	REQUIRE(reportLevel(clean.st.snapshot(true)) == ReportLevel::Debug);   // decimation is intentional

	Sim abandoned; abandoned.frame(1); abandoned.st.restart(); abandoned.st.abandoned();
	REQUIRE(reportLevel(abandoned.st.snapshot(true)) == ReportLevel::Debug);   // expected around a restart

	for (Fate fate : { Fate::WorkerBusy, Fate::DriverError, Fate::Short, Fate::LutNotReady, Fate::Inactive, Fate::Undersized })
	{
		Sim lossy; lossy.frame(1); lossy.frame(2, fate);
		REQUIRE(reportLevel(lossy.st.snapshot(true)) == ReportLevel::Info);
	}
	Sim failed; failed.frame(1); failed.st.decodeFailed();
	REQUIRE(reportLevel(failed.st.snapshot(true)) == ReportLevel::Info);
	Sim gap; gap.frame(1); gap.frame(5);
	REQUIRE(reportLevel(gap.st.snapshot(true)) == ReportLevel::Info);       // driver dropped three
	Sim mismatch; mismatch.st.dequeued();                                    // dequeued, no outcome
	const auto m = mismatch.st.snapshot(true);
	REQUIRE(!m.consistent() && reportLevel(m) == ReportLevel::Info);
	REQUIRE(Stats::format(m, 1.0).find("ACCOUNTING MISMATCH") != std::string::npos);
}

void testFlushContext()
{
	Sim sim; for (uint32_t q = 1; q <= 10; ++q) sim.frame(q);
	const auto s = sim.st.snapshot(true);
	REQUIRE(Stats::format(s, 2.0, "capture stopped").rfind("capture stats (capture stopped): 5.0 fps dequeued", 0) == 0);
	REQUIRE(Stats::format(s, 2.0).rfind("capture stats: 5.0 fps", 0) == 0);
}

void testAcceptTimesAreTiedToTheFrame()
{
	AcceptTimes t; t.resize(2);
	t.accepted(0, 100, 1'000'000);
	REQUIRE(t.completed(0, 100, 1'004'500) == std::optional<uint64_t>(4500));   // matched once
	REQUIRE(!t.completed(0, 100, 1'009'000));                                    // a duplicate yields nothing
	// A late completion of an older frame must not be timed against the newer one.
	t.accepted(1, 200, 2'000'000);
	REQUIRE(!t.completed(1, 199, 2'001'000));
	REQUIRE(t.completed(1, 200, 2'003'000) == std::optional<uint64_t>(3000));    // the real one still works
	REQUIRE(!t.completed(7, 200, 2'003'000));                                    // unknown worker
	t.accepted(7, 1, 0);                                                         // ignored, no crash
	t.accepted(0, 300, 5'000'000); t.clear();
	REQUIRE(!t.completed(0, 300, 5'000'100));                                    // teardown disarms
	t.accepted(0, 301, 6'000'000);
	REQUIRE(t.completed(0, 301, 5'999'000) == std::optional<uint64_t>(0));       // clock step back: 0, not huge
	t.accepted(1, 400, 1); t.resize(4);
	REQUIRE(t.size() == 4 && !t.completed(1, 400, 2));                           // recreated workers start clean
}

void testLifecycleAccountingWithRestarts()
{
	// Mirrors the grabber across restarts: every dequeued buffer has one outcome,
	// and every accepted frame ends decoded, failed or abandoned.
	Rng rng; Sim sim; AcceptTimes times; times.resize(3);
	uint64_t frame = 0, accepted = 0, finished = 0, durations = 0;
	std::vector<uint64_t> inWorker(3, UINT64_MAX);
	uint32_t seq = 0;
	StatsSnapshot total;
	auto drain = [&] {
		const auto s = sim.st.snapshot(true);
		REQUIRE(s.consistent());
		total.dequeued += s.dequeued; total.accepted += s.accepted; total.decoded += s.decoded;
		total.decodeFailed += s.decodeFailed; total.abandoned += s.abandoned; durations += s.durationSamples;
	};
	for (int i = 0; i < 50000; ++i)
	{
		const auto r = rng.next();
		if (r % 4000 == 0)
		{   // restart: frames in workers complete later as abandoned
			sim.st.restart(); times.clear(); seq = 0;
			for (auto& f : inWorker) if (f != UINT64_MAX) { sim.st.abandoned(); ++finished; f = UINT64_MAX; }
			drain();
			continue;
		}
		const size_t w = r % 3;
		if (inWorker[w] != UINT64_MAX)
		{   // this worker finishes its frame
			if ((r >> 8) % 10) { auto us = times.completed(w, inWorker[w], 1000 + (r % 5000)); us ? sim.st.decoded(*us) : sim.st.decoded(); }
			else { times.completed(w, inWorker[w], 0); sim.st.decodeFailed(); }
			inWorker[w] = UINT64_MAX; ++finished;
			continue;
		}
		++frame; ++seq;
		if ((r >> 12) % 3 == 0) { sim.frame(seq, Fate::WorkerBusy); continue; }
		sim.frame(seq, Fate::Accept); ++accepted;
		times.accepted(w, frame, 1000); inWorker[w] = frame;
		if (i % 5000 == 0) drain();
	}
	for (size_t w = 0; w < 3; ++w) if (inWorker[w] != UINT64_MAX) { sim.st.abandoned(); ++finished; }
	drain();
	REQUIRE(total.accepted == accepted && finished == accepted);
	REQUIRE(total.decoded + total.decodeFailed + total.abandoned == total.accepted);
	REQUIRE(durations == total.decoded);   // every decode after a matched accept was timed exactly once
}
}

int main()
{
	testNormalCapture();
	testMissingSequenceNumbers();
	testWorkerBusyAndDecimation();
	testShortFrameDriverErrorAndOtherCauses();
	testRestartAndSequenceReset();
	testCounterResetKeepsSequenceTracking();
	testSequenceWraparound();
	testProcessingFailureAndRecovery();
	testUnavailableIsNotZero();
	testHistogramErrorBound();
	testAccountingInvariantUnderRandomStreams();
	testConcurrentUpdatesAndSnapshots();
	testAbandonedByRestart();
	testDriverWithoutSequenceNumbers();
	testReportLevels();
	testFlushContext();
	testAcceptTimesAreTiedToTheFrame();
	testLifecycleAccountingWithRestarts();
	if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
	std::puts("all V4L2 stats checks passed");
	return 0;
}
