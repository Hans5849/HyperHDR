#pragma once
// Low-overhead V4L2 pipeline counters (issue #9). Header-only and Qt-free so the
// counting rules are tested without hardware (tests/v4l2/stats_test.cpp).
//
// What is counted, per reporting interval, with the exact meaning of each:
//
//   dequeued         every buffer returned by VIDIOC_DQBUF
//   driverError      dequeued with V4L2_BUF_FLAG_ERROR (requeued, not processed)
//   shortPayload     BGR24 payload smaller than the negotiated frame (requeued)
//   decimated        skipped by the software frame decimation setting
//   undersized       process_image's own size check failed (non-BGR24 formats)
//   workersInactive  workers not running (stop or restart in progress)
//   lutNotReady      a LUT is required but still loading
//   workerBusy       no idle worker was available
//   accepted         handed to a decode worker
//   decoded          the worker returned an image
//   decodeFailed     the worker reported an error
//   abandoned        the worker finished after capture restarted (stale stream
//                    generation); the result is discarded, not delivered
//
// Every dequeued buffer is resolved to exactly one of: driverError, shortPayload,
// decimated, undersized, workersInactive, lutNotReady, workerBusy or accepted, so
//   dequeued == their sum                      (consistent(), exact at a snapshot
//                                              taken between frames)
// Accepted frames finish later, possibly in the next interval, so only
//   decoded + decodeFailed + abandoned <= accepted over a stream's lifetime
// is an invariant, not equality per interval. The difference is frames still in
// a worker.
//
// Sequence gaps use the driver's buffer sequence number (v4l2_buffer.sequence),
// not timestamps. The kernel capture timestamp and the userspace dequeue time are
// different clocks and are never mixed: stage durations below are userspace
// steady-clock microseconds only.
//
// "Unavailable" is not "zero": percentiles are absent without samples, and the
// sequence gap count is absent until the driver's sequence number has been seen to
// change (some drivers always report 0). Metrics that aren't instrumented (LED
// output rate, DDP timing) are reported as unavailable, never as 0.
//
// Counters are relaxed atomics: increments are a few nanoseconds, nothing
// allocates, memory is fixed, and a snapshot may be taken from another thread.
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace V4L2Capture
{
// Fixed-size log-linear histogram of microsecond values: 8 buckets per octave, so
// a reported percentile is the bucket's upper bound, at most 12.5% above the true
// value (exact below 8 us). 240 buckets cover 0 .. 2^32 us; larger values clamp.
class DurationHistogram
{
public:
	static constexpr int Buckets = 240;

	void add(uint64_t us)
	{
		_buckets[index(us)].fetch_add(1, std::memory_order_relaxed);
		_sum.fetch_add(us, std::memory_order_relaxed);
	}

	// Moves the interval's samples out and clears them.
	struct Samples
	{
		std::array<uint64_t, Buckets> buckets{};
		uint64_t count = 0, sum = 0;

		std::optional<uint64_t> percentile(double p) const
		{
			if (count == 0) return std::nullopt;
			uint64_t rank = static_cast<uint64_t>(p / 100.0 * static_cast<double>(count) + 0.999999999);
			if (rank < 1) rank = 1;
			if (rank > count) rank = count;
			uint64_t seen = 0;
			for (int i = 0; i < Buckets; ++i)
			{
				seen += buckets[i];
				if (seen >= rank) return upperBound(i);
			}
			return upperBound(Buckets - 1);
		}
		std::optional<double> mean() const
		{
			if (count == 0) return std::nullopt;
			return static_cast<double>(sum) / static_cast<double>(count);
		}
	};

	Samples drain(bool reset)
	{
		Samples s;
		for (int i = 0; i < Buckets; ++i)
		{
			s.buckets[i] = reset ? _buckets[i].exchange(0, std::memory_order_relaxed) : _buckets[i].load(std::memory_order_relaxed);
			s.count += s.buckets[i];
		}
		s.sum = reset ? _sum.exchange(0, std::memory_order_relaxed) : _sum.load(std::memory_order_relaxed);
		return s;
	}

	static int index(uint64_t us)
	{
		if (us < 8) return static_cast<int>(us);
		if (us > 0xffffffffULL) us = 0xffffffffULL;
		int e = 63 - __builtin_clzll(us);                    // floor(log2)
		const int sub = static_cast<int>((us >> (e - 3)) & 7);
		return (e - 2) * 8 + sub;                              // e = 3 -> 8 .. e = 31 -> 239
	}
	static uint64_t upperBound(int bucket)
	{
		if (bucket < 8) return static_cast<uint64_t>(bucket);
		const int e = bucket / 8 + 2, sub = bucket % 8;
		return ((static_cast<uint64_t>(9 + sub)) << (e - 3)) - 1;
	}

private:
	std::array<std::atomic<uint64_t>, Buckets> _buckets{};
	std::atomic<uint64_t> _sum{0};
};

struct StatsSnapshot
{
	uint64_t dequeued = 0, driverError = 0, shortPayload = 0, decimated = 0, undersized = 0,
		workersInactive = 0, lutNotReady = 0, workerBusy = 0, accepted = 0, decoded = 0, decodeFailed = 0, abandoned = 0;
	uint64_t restarts = 0, sequenceResets = 0, duplicateSequences = 0;
	std::optional<uint64_t> missedFrames;                     // from driver sequence gaps; absent if the driver gives no sequence
	std::optional<uint64_t> p50Us, p95Us, p99Us;              // accept -> decoded callback; absent without samples
	std::optional<double> meanUs;
	uint64_t durationSamples = 0;

	uint64_t discarded() const
	{
		return driverError + shortPayload + decimated + undersized + workersInactive + lutNotReady + workerBusy;
	}
	// Frames lost to a cause other than the intentional decimation setting.
	uint64_t dropped() const { return discarded() - decimated; }
	bool consistent() const { return dequeued == discarded() + accepted; }
	// Any frame moved in the interval. Restarts alone (a failed start during
	// backoff) don't count, so retries don't produce a line each.
	bool hasActivity() const
	{
		return dequeued || accepted || decoded || decodeFailed || abandoned;
	}
};

// How loudly an interval is reported: silent with no activity, debug when the
// pipeline was clean, info when frames were lost, failed or miscounted. Frames
// abandoned by a restart are expected (the restart itself is logged) and decimated
// frames are intentional, so neither raises the level.
enum class ReportLevel { None, Debug, Info };

inline ReportLevel reportLevel(const StatsSnapshot& s)
{
	if (!s.hasActivity()) return ReportLevel::None;
	if (s.dropped() > 0 || s.decodeFailed > 0 || (s.missedFrames && *s.missedFrames > 0) || !s.consistent())
		return ReportLevel::Info;
	return ReportLevel::Debug;
}

// Per-worker acceptance times for accept -> decoded durations. A slot remembers
// which frame it timed; a completion only yields a duration for that same frame,
// and only once, so a stale, duplicate or out-of-order callback can never be
// timed against another frame's acceptance.
class AcceptTimes
{
public:
	void resize(size_t workers) { _slots.assign(workers, Slot{}); }
	void accepted(size_t worker, uint64_t frame, int64_t nowUs)
	{
		if (worker < _slots.size()) _slots[worker] = Slot{ frame, nowUs, true };
	}
	std::optional<uint64_t> completed(size_t worker, uint64_t frame, int64_t nowUs)
	{
		if (worker >= _slots.size()) return std::nullopt;
		Slot& slot = _slots[worker];
		if (!slot.armed || slot.frame != frame) return std::nullopt;
		slot.armed = false;
		return nowUs >= slot.atUs ? static_cast<uint64_t>(nowUs - slot.atUs) : 0;
	}
	void clear() { for (auto& slot : _slots) slot.armed = false; }
	size_t size() const { return _slots.size(); }

private:
	struct Slot { uint64_t frame = 0; int64_t atUs = 0; bool armed = false; };
	std::vector<Slot> _slots;
};

class Stats
{
public:
	void dequeued() { inc(_dequeued); }
	void driverError() { inc(_driverError); }
	void shortPayload() { inc(_shortPayload); }
	void decimated() { inc(_decimated); }
	void undersized() { inc(_undersized); }
	void workersInactive() { inc(_workersInactive); }
	void lutNotReady() { inc(_lutNotReady); }
	void workerBusy() { inc(_workerBusy); }
	void accepted() { inc(_accepted); }
	void decoded(uint64_t durationUs) { inc(_decoded); _durations.add(durationUs); }
	void decoded() { inc(_decoded); }                         // when the accept time is unknown: no duration sample
	void decodeFailed() { inc(_decodeFailed); }
	void abandoned() { inc(_abandoned); }

	// Driver sequence number of a dequeued buffer (including error buffers).
	// Forward steps larger than 1 count as missed frames; a repeat or a step
	// backwards is not a gap: it is recorded separately and re-anchors tracking.
	void sequence(uint32_t seq)
	{
		if (!_haveSeq.load(std::memory_order_relaxed))
		{
			_lastSeq.store(seq, std::memory_order_relaxed);
			_haveSeq.store(true, std::memory_order_relaxed);
			return;
		}
		const int32_t delta = static_cast<int32_t>(seq - _lastSeq.load(std::memory_order_relaxed));   // modulo 2^32: wraparound is a +1 step
		_lastSeq.store(seq, std::memory_order_relaxed);
		// Drivers that never fill in the sequence number report 0 every time:
		// a repeat only means something once the number has been seen to move.
		if (delta == 0) { if (_sequenceChanged.load(std::memory_order_relaxed)) inc(_duplicateSequences); return; }
		if (delta < 0) { inc(_sequenceResets); return; }
		_sequenceChanged.store(true, std::memory_order_relaxed);
		if (delta > 1) _missed.fetch_add(static_cast<uint64_t>(delta - 1), std::memory_order_relaxed);
	}

	// A new stream (start, retry or source change): the driver's sequence restarts
	// at 0 and must not be read as a gap. Counters themselves keep running.
	void restart()
	{
		_haveSeq.store(false, std::memory_order_relaxed);
		inc(_restarts);
	}

	// Interval aggregates. With reset, the interval starts over; sequence tracking
	// continues so a reset never invents or hides a gap.
	StatsSnapshot snapshot(bool reset)
	{
		StatsSnapshot s;
		const auto take = [reset](std::atomic<uint64_t>& a) { return reset ? a.exchange(0, std::memory_order_relaxed) : a.load(std::memory_order_relaxed); };
		s.dequeued = take(_dequeued); s.driverError = take(_driverError); s.shortPayload = take(_shortPayload);
		s.decimated = take(_decimated); s.undersized = take(_undersized); s.workersInactive = take(_workersInactive);
		s.lutNotReady = take(_lutNotReady); s.workerBusy = take(_workerBusy); s.accepted = take(_accepted);
		s.decoded = take(_decoded); s.decodeFailed = take(_decodeFailed); s.abandoned = take(_abandoned);
		s.restarts = take(_restarts); s.sequenceResets = take(_sequenceResets); s.duplicateSequences = take(_duplicateSequences);
		const uint64_t missed = take(_missed);
		if (_sequenceChanged.load(std::memory_order_relaxed)) s.missedFrames = missed;
		const auto samples = _durations.drain(reset);
		s.durationSamples = samples.count;
		s.p50Us = samples.percentile(50); s.p95Us = samples.percentile(95); s.p99Us = samples.percentile(99);
		s.meanUs = samples.mean();
		return s;
	}

	// One line for a periodic log. seconds is the interval length; context, when
	// given, says why the interval ended early ("capture stopped").
	static std::string format(const StatsSnapshot& s, double seconds, const std::string& context = {})
	{
		const auto num = [](const std::optional<uint64_t>& v) { return v ? std::to_string(*v) : std::string("n/a"); };
		const auto ms = [](const std::optional<uint64_t>& v) {
			if (!v) return std::string("n/a");
			char b[32]; std::snprintf(b, sizeof b, "%.2f", static_cast<double>(*v) / 1000.0); return std::string(b);
		};
		char rate[32]; std::snprintf(rate, sizeof rate, "%.1f", seconds > 0 ? static_cast<double>(s.dequeued) / seconds : 0.0);
		std::string out = "capture stats" + (context.empty() ? std::string() : " (" + context + ")") + ": " + std::string(rate) + " fps dequeued, " + std::to_string(s.accepted) + " accepted, " +
			std::to_string(s.decoded) + " decoded, " + std::to_string(s.decodeFailed) + " decode errors, " +
			std::to_string(s.abandoned) + " abandoned by restart; discarded " +
			std::to_string(s.discarded()) + " (decimation " + std::to_string(s.decimated) + ", worker busy " + std::to_string(s.workerBusy) +
			", LUT loading " + std::to_string(s.lutNotReady) + ", workers inactive " + std::to_string(s.workersInactive) +
			", driver error " + std::to_string(s.driverError) + ", short payload " + std::to_string(s.shortPayload) +
			", undersized " + std::to_string(s.undersized) + "); missed by driver sequence " + num(s.missedFrames) +
			"; accept->decoded p50/p95/p99 " + ms(s.p50Us) + "/" + ms(s.p95Us) + "/" + ms(s.p99Us) + " ms (" + std::to_string(s.durationSamples) +
			" samples); restarts " + std::to_string(s.restarts) + ", sequence resets " + std::to_string(s.sequenceResets) +
			", duplicate sequences " + std::to_string(s.duplicateSequences) +
			"; LED output rate n/a, DDP timing n/a (not instrumented)";
		if (!s.consistent()) out += "; ACCOUNTING MISMATCH";
		return out;
	}

private:
	static void inc(std::atomic<uint64_t>& a) { a.fetch_add(1, std::memory_order_relaxed); }

	std::atomic<uint64_t> _dequeued{0}, _driverError{0}, _shortPayload{0}, _decimated{0}, _undersized{0}, _workersInactive{0},
		_lutNotReady{0}, _workerBusy{0}, _accepted{0}, _decoded{0}, _decodeFailed{0}, _abandoned{0},
		_restarts{0}, _sequenceResets{0}, _duplicateSequences{0}, _missed{0};
	std::atomic<bool> _haveSeq{false}, _sequenceChanged{false};
	std::atomic<uint32_t> _lastSeq{0};
	DurationHistogram _durations;
};
}
