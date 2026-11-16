#pragma once

// Capture lifecycle decisions for V4L2Grabber, free of Qt so they can be tested
// (issue #19). The supervisor decides when capture is torn down, when a retry is
// armed, when the receiver is watched for a signal, and which late events are
// stale. The host performs those actions with real timers, notifiers and fds.
//
// Steady states and what the host holds in each:
//   Stopped           nothing
//   Streaming         capture (fd, buffers, workers, watchdog)
//   Backoff           one armed retry timer
//   WaitingForSignal  one non-streaming handle on the receiver + its poll timer
// No two of capture, retry timer and signal wait are held at once.

#include <grabber/linux/v4l2/V4L2Capture.h>

#include <string>

namespace V4L2Capture
{

enum class LogLevel { Debug, Info, Warning };

// Thrown by SupervisorHost::startCapture for anything other than NoSignal.
struct CaptureError : std::runtime_error
{
	CaptureError(Failure stage, const std::string& detail) : std::runtime_error(detail), failure(stage) {}
	Failure failure;
};

class SupervisorHost
{
public:
	virtual ~SupervisorHost() = default;
	// Discovery, initialization and STREAMON. Throws NoSignal when the receiver
	// reports no input, CaptureError (or any std::exception) otherwise.
	virtual void startCapture() = 0;
	// Release streaming capture completely. Must be idempotent.
	virtual void stopCapture() = 0;
	// Single-shot retry; replaces any pending one.
	virtual void armRetry(int delayMs) = 0;
	virtual void cancelRetry() = 0;
	// Open a non-streaming, event-subscribed handle on node and start polling
	// every Recovery::signalPollMs. Returns false when the node cannot be opened;
	// polling still starts, and the next poll falls back to full discovery.
	virtual bool openSignalWait(const std::string& node) = 0;
	// Close the handle and stop polling. Must be idempotent.
	virtual void closeSignalWait() = 0;
	// Drain pending events on the waiting handle and probe its signal.
	virtual SignalStatus probeWaitingSignal() = 0;
	// /dev/videoN of the most recently opened capture node.
	virtual std::string activeNode() = 0;
	virtual void log(LogLevel level, const std::string& message) = 0;
};

class Supervisor
{
public:
	enum class State { Stopped, Streaming, Backoff, WaitingForSignal };

	explicit Supervisor(SupervisorHost& host) : _host(host) {}

	State state() const { return _state; }
	// True while startCapture runs for an automatic recovery (retry timer or
	// signal return); the grabber then reuses the LUT already in memory.
	bool retrying() const { return _retrying; }
	const Recovery& recovery() const { return _recovery; }

	// Explicit start (enable, settings change): forgets earlier failures.
	bool start() { return attempt(false); }

	// Explicit stop: cancels every retry and wait and releases capture.
	void stop()
	{
		_decodeFailures = 0;
		_awaitingDecode = false;
		_host.cancelRetry();
		_host.closeSignalWait();
		_host.stopCapture();
		_waitHandle = false;
		_state = State::Stopped;
	}

	void retryTimerFired()
	{
		if (_state == State::Backoff)
			attempt(true);
	}

	// Poll timer or source-change event on the waiting handle.
	void signalPollFired()
	{
		if (_state != State::WaitingForSignal)
			return;   // stale timer or event after stop/start
		if (_waitHandle)
		{
			const auto status = _host.probeWaitingSignal();
			if (status.state == Signal::Absent)
			{
				noteNoSignal(status.detail);
				return;
			}
			_host.log(LogLevel::Info, "Input signal on " + _node + (status.detail.empty() ? "" : " (" + status.detail + ")") +
				"; starting capture");
		}
		else
			_host.log(LogLevel::Info, "Cannot reopen " + _node + " to check its input; rediscovering capture devices");
		attempt(true);
	}

	// Watchdog: no complete frame for five seconds. captureSignal is the
	// probe on the streaming fd.
	void stalled(const SignalStatus& captureSignal)
	{
		if (_state != State::Streaming)
			return;
		if (captureSignal.state == Signal::Absent)
			return enterWait(captureSignal.detail);
		fail(Failure::Timeout, captureSignal.detail.empty() ? "no frames for five seconds" :
			"no frames for five seconds (receiver: " + captureSignal.detail + ")");
	}

	// V4L2 event notifier on the streaming fd.
	void sourceEvent(bool sourceChanged, const SignalStatus& captureSignal)
	{
		if (_state != State::Streaming)
			return;
		if (captureSignal.state == Signal::Absent)
			return enterWait(captureSignal.detail);
		if (sourceChanged)
			fail(Failure::SourceChange, captureSignal.detail.empty() ? "input timing or format changed" : captureSignal.detail, true);
		else
			fail(Failure::Streaming, "source event notification without a readable event");
	}

	// DQBUF/QBUF failure, invalid payload, repeated short frames, worker error.
	void streamFailed(const std::string& detail)
	{
		if (_state == State::Streaming)
			fail(Failure::Streaming, detail);
	}

	// A complete frame was dequeued. Logs recovery after earlier failed
	// attempts, except after a decode-failure restart: then only a frame that
	// actually decodes proves recovery (decodeSucceeded), so a stream that
	// delivers buffers but never decodes keeps backing off instead of
	// resetting to the shortest delay on every dequeue.
	void frameReceived()
	{
		if (_awaitingDecode) { _recovery.payloadOk(); return; }
		recovered();
	}

	// Decode health. A stream that keeps delivering buffers that never decode
	// must not stay "healthy" (the watchdog only sees dequeues). It trips only
	// when both hold: decodeFailureLimit consecutive failures, spanning at least
	// decodeFailureWindowMs with no successful decode in between. Isolated errors
	// (a corrupt MJPEG frame) reset on the next success and never restart.
	static constexpr unsigned decodeFailureLimit = 30;
	static constexpr int64_t decodeFailureWindowMs = 5000;

	void decodeFailed(int64_t nowMs, const std::string& error = {})
	{
		if (_state != State::Streaming)
			return;   // late worker result after a stop or restart
		if (_decodeFailures++ == 0)
			_firstDecodeFailureMs = nowMs;
		const int64_t span = nowMs - _firstDecodeFailureMs;
		if (_decodeFailures < decodeFailureLimit || span < decodeFailureWindowMs)
			return;
		const std::string detail = "decoding: " + std::to_string(_decodeFailures) +
			" consecutive frames failed to decode over " + std::to_string(span) + " ms" +
			(error.empty() ? std::string() : " (last error: " + error + ")");
		_decodeFailures = 0;
		_awaitingDecode = true;
		fail(Failure::Streaming, detail);
	}

	void decodeSucceeded()
	{
		_decodeFailures = 0;
		if (_awaitingDecode)
		{
			_awaitingDecode = false;
			recovered();
		}
	}

	unsigned consecutiveDecodeFailures() const { return _decodeFailures; }
	bool awaitingDecode() const { return _awaitingDecode; }

	// True when this short frame should escalate to a restart.
	bool shortFrame() { return _recovery.shortFrame(); }

private:
	bool attempt(bool automatic)
	{
		if (_state == State::Streaming)
			return true;
		_host.cancelRetry();
		_host.closeSignalWait();
		_waitHandle = false;
		if (!automatic)
		{
			_recovery = {};
			_awaitingDecode = false;
		}
		_decodeFailures = 0;   // a new stream starts with a clean decode record
		_retrying = automatic;
		try
		{
			_host.startCapture();
			_retrying = false;
			_state = State::Streaming;
			return true;
		}
		catch (const NoSignal& e)
		{
			_retrying = false;
			enterWait(e.what());
		}
		catch (const CaptureError& e)
		{
			_retrying = false;
			_state = State::Streaming;   // let fail() tear down from a known state
			fail(e.failure, e.what());
		}
		catch (const std::exception& e)
		{
			_retrying = false;
			_state = State::Streaming;
			fail(Failure::Initialization, e.what());
		}
		return false;
	}

	void recovered()
	{
		if (const unsigned failures = _recovery.frameReceived())
			_host.log(LogLevel::Info, "Capture recovered after " + std::to_string(failures) + " failed attempt(s)");
	}

	void fail(Failure failure, const std::string& detail, bool sourceChanged = false)
	{
		_host.stopCapture();
		const int delay = _recovery.nextDelay(sourceChanged);
		const std::string message = describe(failure, detail);
		// Loud once per outage; repeats while the source stays dark are debug.
		if (!_recovery.shouldWarn(message))
			_host.log(LogLevel::Debug, message + "; retry " + std::to_string(_recovery.failures()) + " in " + std::to_string(delay) + " ms");
		else if (failure == Failure::SourceChange)
			_host.log(LogLevel::Info, message + "; restarting capture in " + std::to_string(delay) + " ms");
		else
			_host.log(LogLevel::Warning, message + "; retrying capture in " + std::to_string(delay) + " ms");
		_host.armRetry(delay);
		_state = State::Backoff;
	}

	void enterWait(const std::string& detail)
	{
		_host.stopCapture();
		_host.cancelRetry();
		_node = _host.activeNode();
		_state = State::WaitingForSignal;
		noteNoSignal(detail, true);
		// An empty or vanished node returns false; polling still runs and the
		// next poll falls back to full discovery.
		_waitHandle = _host.openSignalWait(_node);
	}

	void noteNoSignal(const std::string& detail, bool entering = false)
	{
		const std::string message = describe(Failure::NoSignal, detail);
		if (_recovery.shouldWarn(message))
			_host.log(LogLevel::Info, message + " on " + _node + "; capture paused, checking every " +
				std::to_string(Recovery::signalPollMs) + " ms");
		else
			_host.log(LogLevel::Debug, message + " on " + _node + (entering ? "; waiting" : "; still waiting"));
	}

	SupervisorHost& _host;
	State _state = State::Stopped;
	Recovery _recovery;
	bool _retrying = false;
	bool _waitHandle = false;
	std::string _node;
	unsigned _decodeFailures = 0;
	int64_t _firstDecodeFailureMs = 0;
	bool _awaitingDecode = false;
};

}
