#pragma once
// LED output counters (issue #9). Header-only and Qt-free so the counting rules
// are tested without a device or network (tests/led_output).
//
// What is counted, per reporting interval:
//
//   presented   final LED colours handed to the device (LedDevice::
//               handleSignalFinalOutputColorsReady). With smoothing these come
//               from the smoothing clock, not from captured frames, so there is
//               no 1:1 relation between captured frames and presented updates.
//   inactive    presented while the device was disabled, off, not ready or in
//               error; discarded
//   superseded  presented, then replaced by newer colours before they were
//               written (no refresh timer: updates are queued and coalesced)
//   updates     update writes started by rewriteLEDs. Each is ONE logical output
//               group, however many packets the driver splits it into.
//   repeats     updates that resent colours already written (refresh timer
//               without smoothing, identify blink); included in updates
//   failed      updates where the driver returned an error or any packet of the
//               update failed
//   unsent      updates during which a packet-counting (UDP) device sent no
//               datagram although the driver did not report an error (e.g. the
//               wled driver adopting a new LED count returns 0 and sends
//               nothing). Driver acceptance is not transmission.
//   packets     datagrams the UDP socket accepted or rejected during updates
//   packetFailures  of those, rejected (writeDatagram error or short write)
//   bytes       datagram bytes accepted during updates (headers included)
//   otherPackets    datagrams outside an update: black on stop, protocol or
//               keep-alive messages. Not LED updates.
//
// "Accepted" means the local socket took the datagram. It is not delivery to the
// controller, and not light from the LEDs: UDP has no acknowledgement, and the
// controller may drop, delay or not display a frame. None of these counters is a
// capture-to-LED latency.
//
// Accounting identity, exact at a snapshot taken between updates, over the
// counters' lifetime (pendingFresh is 0 or 1: presented colours not yet written):
//   presented == inactive + superseded + (updates - repeats) + pendingFresh
//
// Counters are relaxed atomics: presented() may run on another thread than the
// device; everything else runs on the device thread.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace LedOutput
{
struct Snapshot
{
	uint64_t presented = 0;
	uint64_t inactive = 0;
	uint64_t superseded = 0;
	uint64_t updates = 0;
	uint64_t repeats = 0;
	uint64_t failed = 0;
	uint64_t packets = 0;
	uint64_t packetFailures = 0;
	uint64_t bytes = 0;
	uint64_t otherPackets = 0;
	uint64_t otherPacketFailures = 0;
	uint64_t unsent = 0;
	uint64_t maxPacketsPerUpdate = 0;   // interval maximum, not a difference
	bool packetsCounted = false;        // false: the driver doesn't send through
	                                    // ProviderUdp; packet fields are unavailable

	uint64_t freshUpdates() const { return updates - repeats; }
	bool hasActivity() const { return presented || updates || packets || otherPackets; }

	Snapshot operator-(const Snapshot& b) const
	{
		Snapshot d;
		d.presented = presented - b.presented;
		d.inactive = inactive - b.inactive;
		d.superseded = superseded - b.superseded;
		d.updates = updates - b.updates;
		d.repeats = repeats - b.repeats;
		d.failed = failed - b.failed;
		d.packets = packets - b.packets;
		d.packetFailures = packetFailures - b.packetFailures;
		d.bytes = bytes - b.bytes;
		d.otherPackets = otherPackets - b.otherPackets;
		d.otherPacketFailures = otherPacketFailures - b.otherPacketFailures;
		d.unsent = unsent - b.unsent;
		d.maxPacketsPerUpdate = maxPacketsPerUpdate;
		d.packetsCounted = packetsCounted;
		return d;
	}
};

enum class ReportLevel { Debug, Info };

// Failures are worth a log line at the default level; a healthy or idle interval
// is debug only. An idle interval is still reported (as zero output), so a
// device that stopped receiving colours is visible.
inline ReportLevel reportLevel(const Snapshot& s)
{
	return (s.failed || s.packetFailures || s.otherPacketFailures || s.unsent) ? ReportLevel::Info : ReportLevel::Debug;
}

class Stats
{
public:
	static constexpr int64_t intervalMs = 60000;

	// Set by ProviderUdp: this device's datagrams are counted. Other drivers
	// (serial, SPI, HID, REST) report packets as unavailable, not as 0.
	void countPackets() { _packetsCounted = true; }

	// Final colours arrived. active: the device can write them.
	void presented(bool active)
	{
		inc(_presented);
		if (!active)
		{
			inc(_inactive);
			return;
		}
		if (_fresh.exchange(true, std::memory_order_relaxed))
			inc(_superseded);
	}

	// rewriteLEDs is about to write one update.
	void beginUpdate()
	{
		inc(_updates);
		if (!_fresh.exchange(false, std::memory_order_relaxed))
			inc(_repeats);
		_inUpdate = true;
		_updatePackets = 0;
		_updatePacketFailed = false;
	}

	// One datagram handed to the socket; ok when it was accepted whole.
	void packet(uint64_t size, bool ok)
	{
		if (_inUpdate)
		{
			inc(_packets);
			++_updatePackets;
			if (ok)
				_bytes.fetch_add(size, std::memory_order_relaxed);
			else
			{
				inc(_packetFailures);
				_updatePacketFailed = true;
			}
		}
		else
		{
			inc(_otherPackets);
			if (!ok)
				inc(_otherPacketFailures);
		}
	}

	// The driver's write returned writeResult (negative: error).
	void endUpdate(int writeResult)
	{
		if (!_inUpdate)
			return;
		_inUpdate = false;
		if (writeResult < 0 || _updatePacketFailed)
			inc(_failed);
		else if (_packetsCounted && _updatePackets == 0)
			inc(_unsent);
		if (_updatePackets > _maxPackets.load(std::memory_order_relaxed))
			_maxPackets.store(_updatePackets, std::memory_order_relaxed);
	}

	// Device stopped or restarted: colours presented earlier will not be written.
	void restart()
	{
		if (_fresh.exchange(false, std::memory_order_relaxed))
			inc(_superseded);
		_inUpdate = false;
	}

	bool pendingFresh() const { return _fresh.load(std::memory_order_relaxed); }

	Snapshot snapshot() const
	{
		Snapshot s;
		s.presented = get(_presented);
		s.inactive = get(_inactive);
		s.superseded = get(_superseded);
		s.updates = get(_updates);
		s.repeats = get(_repeats);
		s.failed = get(_failed);
		s.packets = get(_packets);
		s.packetFailures = get(_packetFailures);
		s.bytes = get(_bytes);
		s.otherPackets = get(_otherPackets);
		s.otherPacketFailures = get(_otherPacketFailures);
		s.unsent = get(_unsent);
		s.maxPacketsPerUpdate = get(_maxPackets);
		s.packetsCounted = _packetsCounted;
		return s;
	}

	// Lifetime identity (see the header comment).
	bool consistent() const
	{
		const Snapshot s = snapshot();
		return s.presented == s.inactive + s.superseded + s.freshUpdates() + (pendingFresh() ? 1 : 0);
	}

	// Interval bookkeeping. begin() starts an interval; due() tells the periodic
	// timer whether one has elapsed; take() returns the interval's counters and
	// starts the next one.
	void begin(int64_t nowMs)
	{
		_intervalBegin = nowMs;
		_last = snapshot();
		_maxPackets.store(0, std::memory_order_relaxed);
	}
	bool running() const { return _intervalBegin >= 0; }

	// Start reporting (device enabled). When an interval is still running (the
	// device went into error without being disabled and is now re-enabled), its
	// counters are returned for one last report instead of being discarded.
	std::optional<std::pair<double, Snapshot>> start(int64_t nowMs)
	{
		if (running())
			return take(nowMs);
		begin(nowMs);
		return std::nullopt;
	}
	bool due(int64_t nowMs) const { return running() && nowMs - _intervalBegin >= intervalMs; }
	void end() { _intervalBegin = -1; }

	// Seconds covered and the counters since begin(). Starts the next interval.
	std::pair<double, Snapshot> take(int64_t nowMs)
	{
		const double seconds = running() ? std::max<int64_t>(nowMs - _intervalBegin, 0) / 1000.0 : 0.0;
		const Snapshot now = snapshot();
		const Snapshot d = now - _last;
		begin(nowMs);
		return { seconds, d };
	}

	// One log line. Rates are per second over the interval; "n/a" when the
	// interval has no duration (never 0 for an unknown rate).
	static std::string format(const Snapshot& s, double seconds, const std::string& context = {})
	{
		std::string out = "LED output stats";
		if (!context.empty())
			out += " (" + context + ")";
		out += ": " + fixed(seconds, 1) + " s";
		if (!s.hasActivity())
			return out + ", no LED output (0 presented, 0 updates)";
		out += ", presented " + num(s.presented) + rate(s.presented, seconds);
		if (s.inactive)
			out += ", " + num(s.inactive) + " while inactive";
		if (s.superseded)
			out += ", " + num(s.superseded) + " superseded";
		out += "; updates " + num(s.updates) + rate(s.updates, seconds);
		out += " (new colours " + num(s.freshUpdates()) + ", repeats " + num(s.repeats) + ")";
		out += ", failed " + num(s.failed);
		if (s.unsent)
			out += ", " + num(s.unsent) + " sent no packet";
		if (!s.packetsCounted)
			return out + "; packets n/a (driver not using the UDP provider)";
		out += "; packets " + num(s.packets);
		if (s.updates)
			out += " (" + fixed(static_cast<double>(s.packets) / static_cast<double>(s.updates), 2) + "/update, max " + num(s.maxPacketsPerUpdate) + ")";
		out += ", rejected by socket " + num(s.packetFailures);
		out += ", " + num(s.bytes) + " bytes accepted";
		if (s.otherPackets)
			out += "; outside updates " + num(s.otherPackets) + " packets (" + num(s.otherPacketFailures) + " rejected)";
		return out;
	}

private:
	static void inc(std::atomic<uint64_t>& c) { c.fetch_add(1, std::memory_order_relaxed); }
	static uint64_t get(const std::atomic<uint64_t>& c) { return c.load(std::memory_order_relaxed); }
	static std::string num(uint64_t v) { return std::to_string(v); }
	static std::string fixed(double v, int decimals)
	{
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
		return buf;
	}
	static std::string rate(uint64_t count, double seconds)
	{
		return seconds > 0 ? " (" + fixed(static_cast<double>(count) / seconds, 2) + "/s)" : " (n/a/s)";
	}

	std::atomic<uint64_t> _presented{ 0 }, _inactive{ 0 }, _superseded{ 0 }, _updates{ 0 }, _repeats{ 0 }, _failed{ 0 };
	std::atomic<uint64_t> _packets{ 0 }, _packetFailures{ 0 }, _bytes{ 0 }, _otherPackets{ 0 }, _otherPacketFailures{ 0 }, _unsent{ 0 };
	std::atomic<uint64_t> _maxPackets{ 0 };
	std::atomic<bool> _fresh{ false };
	bool _packetsCounted = false;
	bool _inUpdate = false;
	uint64_t _updatePackets = 0;
	bool _updatePacketFailed = false;
	int64_t _intervalBegin = -1;
	Snapshot _last;
};
}
