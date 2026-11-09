#pragma once

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

// Linux-only capture primitives. Syscalls are injectable so CI needs no device.
namespace V4L2Capture
{
inline __u32 capabilities(const v4l2_capability& cap)
{
	return (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
}

inline v4l2_buf_type captureType(const v4l2_capability& cap)
{
	const auto caps = capabilities(cap);
	// Codec, scaler and RGA nodes are memory-to-memory devices, never sources.
	if (caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE))
		throw std::runtime_error("discovery: memory-to-memory (codec/scaler) node");
	if (!(caps & V4L2_CAP_STREAMING))
		throw std::runtime_error("discovery: device does not support streaming");
	if (caps & V4L2_CAP_VIDEO_CAPTURE)
		return V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
		return V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	throw std::runtime_error("discovery: device has no video capture capability");
}

struct Format
{
	__u32 width, height, fourcc, stride, size, planes, quantization;
};

inline Format format(const v4l2_format& fmt)
{
	if (fmt.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
	{
		const auto& p = fmt.fmt.pix_mp;
		return {p.width, p.height, p.pixelformat, p.plane_fmt[0].bytesperline,
			p.plane_fmt[0].sizeimage, p.num_planes, p.quantization};
	}
	const auto& p = fmt.fmt.pix;
	return {p.width, p.height, p.pixelformat, p.bytesperline, p.sizeimage, 1, p.quantization};
}

inline void requestFormat(v4l2_format& fmt, __u32 width, __u32 height, __u32 fourcc)
{
	if (fmt.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
	{
		fmt.fmt.pix_mp = {};
		fmt.fmt.pix_mp.width = width;
		fmt.fmt.pix_mp.height = height;
		fmt.fmt.pix_mp.pixelformat = fourcc;
		fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
	}
	else
	{
		fmt.fmt.pix.width = width;
		fmt.fmt.pix.height = height;
		fmt.fmt.pix.pixelformat = fourcc;
		if (fourcc == V4L2_PIX_FMT_MJPEG)
			fmt.fmt.pix.field = V4L2_FIELD_ANY;
	}
}

inline void validateFormat(const Format& f)
{
	if (f.planes != 1)
		throw std::runtime_error("format negotiation: decoder requires one contiguous memory plane");
	if (!f.width || !f.height || f.width > INT_MAX || f.height > INT_MAX || f.stride > INT_MAX || f.size > INT_MAX)
		throw std::runtime_error("format negotiation: invalid dimensions or buffer size");
	if (f.fourcc == V4L2_PIX_FMT_BGR24)
	{
		const uint64_t row = uint64_t(f.width) * 3;
		const uint64_t required = uint64_t(f.stride) * (f.height - 1) + row;
		if (f.stride < row || required > f.size || required > INT_MAX)
			throw std::runtime_error("format negotiation: invalid BGR24 stride or sizeimage");
	}
}

// Pure restart/backoff policy, kept free of Qt and syscalls so it is testable.
// - Retries back off 3 s, 6 s, 12 s ... up to 30 s while the source stays dark.
// - A source-change event retries quickly and resets the backoff.
// - Isolated short payloads are dropped; only a run of them forces a restart.
// - The same failure reason is logged loudly once per outage, then quietly.
class Recovery
{
public:
	static constexpr int initialDelayMs = 3000;
	static constexpr int maximumDelayMs = 30000;
	static constexpr int sourceChangeDelayMs = 500;
	static constexpr unsigned shortFrameLimit = 3;
	// While a receiver reports no input, capture stays stopped (no buffers)
	// and the open node is re-probed at this fixed, cheap interval.
	static constexpr int signalPollMs = 2000;

	// Delay before the next start attempt after a failure.
	int nextDelay(bool sourceChanged)
	{
		++_failures;
		if (sourceChanged)
		{
			_delay = initialDelayMs;
			return sourceChangeDelayMs;
		}
		const int delay = _delay;
		_delay = (_delay >= maximumDelayMs / 2) ? maximumDelayMs : _delay * 2;
		return delay;
	}

	// True when this reason should be logged as a warning (first occurrence in
	// the current outage, or a different reason than last time).
	bool shouldWarn(const std::string& reason)
	{
		const bool warn = reason != _lastReason;
		_lastReason = reason;
		return warn;
	}

	// True when a short frame should escalate to a capture restart.
	bool shortFrame() { return ++_shortFrames >= shortFrameLimit; }

	// A complete frame arrived. Returns the number of failed attempts that
	// preceded it (0 when capture was already healthy).
	unsigned frameReceived()
	{
		const unsigned failures = _failures;
		_failures = 0; _shortFrames = 0; _delay = initialDelayMs; _lastReason.clear();
		return failures;
	}

	unsigned failures() const { return _failures; }

private:
	int _delay = initialDelayMs;
	unsigned _failures = 0;
	unsigned _shortFrames = 0;
	std::string _lastReason;
};

class Operations
{
public:
	virtual ~Operations() = default;
	virtual int control(int fd, unsigned long request, void* arg)
	{
		int result;
		do { result = ::ioctl(fd, request, arg); } while (result < 0 && errno == EINTR);
		return result;
	}
	virtual void* map(int fd, size_t size, off_t offset)
	{
		return ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
	}
	virtual int unmap(void* address, size_t size) { return ::munmap(address, size); }
	virtual int open(const char* path) { return ::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC); }
	virtual int close(int fd) { return ::close(fd); }
};

inline void check(int result, const char* stage)
{
	if (result < 0)
		throw std::runtime_error(std::string(stage) + ": " + std::strerror(errno));
}

inline Format negotiate(Operations& ops, int fd, v4l2_format& fmt)
{
	check(ops.control(fd, VIDIOC_S_FMT, &fmt), "format negotiation: VIDIOC_S_FMT");
	const auto result = format(fmt);
	validateFormat(result);
	return result;
}

// Probe advertised modes without changing the device. Drivers may advertise a
// FOURCC that they reject, or silently substitute another one in TRY_FMT.
inline bool tryFormat(Operations& ops, int fd, v4l2_buf_type type, __u32 width, __u32 height, __u32 fourcc)
{
	v4l2_format fmt{}; fmt.type = type;
	requestFormat(fmt, width, height, fourcc);
	if (ops.control(fd, VIDIOC_TRY_FMT, &fmt) < 0) return false;
	const auto f = format(fmt);
	if (f.fourcc != fourcc || f.width != width || f.height != height) return false;
	try { validateFormat(f); } catch (const std::runtime_error&) { return false; }
	return true;
}

struct Descriptor
{
	v4l2_buffer buffer{};
	std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
	Descriptor(v4l2_buf_type type, unsigned count, unsigned index = 0)
	{
		buffer.type = type;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = index;
		if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		{
			buffer.m.planes = planes.data();
			buffer.length = count;
		}
	}
	Descriptor(const Descriptor&) = delete;
	Descriptor& operator=(const Descriptor&) = delete;
};

class Buffers
{
public:
	struct Mapping { void* data; size_t size; };
	Operations& ops;
	int fd = -1;
	v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	unsigned planeCount = 1;
	std::vector<std::vector<Mapping>> mappings;
	bool streaming = false;
	bool allocated = false;

	explicit Buffers(Operations& operations) : ops(operations) {}
	~Buffers() { clear(); }
	Buffers(const Buffers&) = delete;
	Buffers& operator=(const Buffers&) = delete;

	void allocate(int descriptor, v4l2_buf_type bufferType, unsigned planes)
	{
		clear();
		fd = descriptor; type = bufferType; planeCount = planes;
		if (!planes || planes > VIDEO_MAX_PLANES || (type == V4L2_BUF_TYPE_VIDEO_CAPTURE && planes != 1))
			throw std::runtime_error("buffer setup: invalid plane count");
		try
		{
			v4l2_requestbuffers req{};
			req.count = 4; req.type = type; req.memory = V4L2_MEMORY_MMAP;
			check(ops.control(fd, VIDIOC_REQBUFS, &req), "buffer setup: VIDIOC_REQBUFS");
			allocated = true;
			if (req.count < 2 || req.count > 32)
				throw std::runtime_error("buffer setup: invalid buffer count (expected 2..32)");
			mappings.resize(req.count);
			for (unsigned i = 0; i < req.count; ++i)
			{
				Descriptor d(type, planeCount, i);
				check(ops.control(fd, VIDIOC_QUERYBUF, &d.buffer), "buffer setup: VIDIOC_QUERYBUF");
				if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && d.buffer.length != planeCount)
					throw std::runtime_error("buffer setup: unexpected QUERYBUF plane count");
				for (unsigned p = 0; p < planeCount; ++p)
				{
					const size_t size = type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? d.planes[p].length : d.buffer.length;
					const off_t offset = type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? d.planes[p].m.mem_offset : d.buffer.m.offset;
					if (!size) throw std::runtime_error("buffer setup: empty plane mapping");
					void* address = ops.map(fd, size, offset);
					if (address == MAP_FAILED) check(-1, "buffer setup: mmap");
					try { mappings[i].push_back({address, size}); }
					catch (...) { ops.unmap(address, size); throw; }
				}
			}
		}
		catch (...) { clear(); throw; }
	}

	void queue(unsigned index)
	{
		if (index >= mappings.size()) throw std::runtime_error("streaming: invalid QBUF index");
		Descriptor d(type, planeCount, index);
		for (unsigned p = 0; p < planeCount; ++p) d.planes[p].length = mappings[index][p].size;
		check(ops.control(fd, VIDIOC_QBUF, &d.buffer), "streaming: VIDIOC_QBUF");
	}

	void start()
	{
		for (unsigned i = 0; i < mappings.size(); ++i) queue(i);
		check(ops.control(fd, VIDIOC_STREAMON, &type), "streaming: VIDIOC_STREAMON");
		streaming = true;
	}

	bool dequeue(Descriptor& d)
	{
		if (ops.control(fd, VIDIOC_DQBUF, &d.buffer) < 0)
		{
			if (errno == EAGAIN) return false;
			check(-1, "streaming: VIDIOC_DQBUF");
		}
		if (d.buffer.index >= mappings.size() ||
			(type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && d.buffer.length != planeCount))
			throw std::runtime_error("streaming: invalid dequeued index or plane count");
		for (unsigned p = 0; p < planeCount; ++p)
		{
			const size_t used = type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? d.planes[p].bytesused : d.buffer.bytesused;
			const size_t offset = type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? d.planes[p].data_offset : 0;
			if (used > mappings[d.buffer.index][p].size || offset > used || used - offset > INT_MAX)
				throw std::runtime_error("streaming: invalid bytesused or data_offset");
		}
		return true;
	}

	// Always try every unmap and REQBUFS(0), including after partial setup.
	bool clear() noexcept
	{
		bool ok = true;
		if (streaming) { ok = ops.control(fd, VIDIOC_STREAMOFF, &type) == 0; streaming = false; }
		for (auto& buffer : mappings)
			for (auto& plane : buffer) if (ops.unmap(plane.data, plane.size) < 0) ok = false;
		mappings.clear();
		if (allocated)
		{
			v4l2_requestbuffers req{}; req.type = type; req.memory = V4L2_MEMORY_MMAP;
			if (ops.control(fd, VIDIOC_REQBUFS, &req) < 0) ok = false;
			allocated = false;
		}
		fd = -1;
		return ok;
	}
};

// ---------------------------------------------------------------------------
// Failure classification for logs.
enum class Failure { NoSignal, Timeout, Initialization, Streaming, SourceChange };

inline const char* label(Failure failure)
{
	switch (failure)
	{
		case Failure::NoSignal: return "no signal";
		case Failure::Timeout: return "timeout";
		case Failure::Initialization: return "initialization failed";
		case Failure::Streaming: return "streaming failed";
		case Failure::SourceChange: return "source changed";
	}
	return "capture failed";
}

// "<category>: <detail>", without repeating a stage prefix such as
// "streaming: " that already names the category.
inline std::string describe(Failure failure, std::string detail)
{
	if (failure == Failure::Streaming && detail.rfind("streaming: ", 0) == 0)
		detail.erase(0, 11);
	return std::string(label(failure)) + ": " + detail;
}

// Thrown during initialization when the receiver itself reports no input.
struct NoSignal : std::runtime_error
{
	using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// Input signal state, from generic V4L2 receiver interfaces only.
//
// - V4L2_CID_DV_RX_POWER_PRESENT (read-only bitmask): 0 means no source +5V.
// - VIDIOC_QUERY_DV_TIMINGS: ENOLINK no signal, ENOLCK unstable, ERANGE
//   out of range.
// Devices exposing neither (USB/UVC and analog grabbers) report Unknown, and
// callers keep their previous behaviour for them. No driver names are used.
enum class Signal { Unknown, Present, Absent };

struct SignalStatus
{
	Signal state = Signal::Unknown;
	std::string detail;
};

inline SignalStatus probeSignal(Operations& ops, int fd)
{
	v4l2_control power{};
	power.id = V4L2_CID_DV_RX_POWER_PRESENT;
	const bool hasPower = ops.control(fd, VIDIOC_G_CTRL, &power) == 0;
	if (hasPower && power.value == 0)
		return { Signal::Absent, "no source detected (power_present=0)" };

	v4l2_dv_timings timings{};
	if (ops.control(fd, VIDIOC_QUERY_DV_TIMINGS, &timings) == 0)
	{
		if (timings.type == V4L2_DV_BT_656_1120 && timings.bt.width && timings.bt.height)
			return { Signal::Present, "detected " + std::to_string(timings.bt.width) + "x" + std::to_string(timings.bt.height) +
				(timings.bt.interlaced ? "i" : "p") };
		return { Signal::Present, "DV timings detected" };
	}
	const std::string power5v = hasPower ? "source detected, " : "";
	switch (errno)
	{
		case ENOLINK: return { Signal::Absent, power5v + "no signal (QUERY_DV_TIMINGS: ENOLINK)" };
		case ENOLCK: return { Signal::Absent, power5v + "signal not locked (QUERY_DV_TIMINGS: ENOLCK)" };
		case ERANGE: return { Signal::Absent, power5v + "signal timings out of range (QUERY_DV_TIMINGS: ERANGE)" };
		default: break;
	}
	return { Signal::Unknown, hasPower ? "source detected (power_present set), signal state not reported" : "" };
}

// ---------------------------------------------------------------------------
// Discovery helpers. Nothing here depends on /dev/videoN numbering.

struct Node
{
	v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	v4l2_format current{};
};

// Classify an opened node without changing it. Returns false, with a reason,
// for codec/RGA/metadata/output nodes and for nodes whose format is unreadable.
inline bool probeNode(Operations& ops, int fd, Node& node, std::string& reason)
{
	v4l2_capability cap{};
	if (ops.control(fd, VIDIOC_QUERYCAP, &cap) < 0)
	{
		reason = std::string("discovery: not a V4L2 device: ") + std::strerror(errno);
		return false;
	}
	try { node.type = captureType(cap); }
	catch (const std::runtime_error& e) { reason = e.what(); return false; }
	node.current = {};
	node.current.type = node.type;
	if (ops.control(fd, VIDIOC_G_FMT, &node.current) < 0)
	{
		reason = std::string("discovery: VIDIOC_G_FMT failed: ") + std::strerror(errno);
		return false;
	}
	return true;
}

struct Mode
{
	__u32 width, height, fourcc;
};

// Native receivers may enumerate neither frame sizes nor inputs. Offer every
// advertised, decodable FOURCC that TRY_FMT accepts at the current timing,
// BGR24 first. Works without a signal: the current timing is whatever the
// driver reports (its default or the last detected one).
inline std::vector<Mode> currentTimingModes(Operations& ops, int fd, const Node& node,
	const std::function<bool(__u32)>& decodable)
{
	std::vector<Mode> modes;
	if (node.type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return modes;
	const auto active = format(node.current);
	v4l2_fmtdesc candidate{};
	candidate.type = node.type;
	while (ops.control(fd, VIDIOC_ENUM_FMT, &candidate) == 0 && candidate.index < 64)
	{
		const __u32 fourcc = candidate.pixelformat;
		if (decodable(fourcc) && tryFormat(ops, fd, node.type, active.width, active.height, fourcc))
		{
			const Mode mode{ active.width, active.height, fourcc };
			if (fourcc == V4L2_PIX_FMT_BGR24) modes.insert(modes.begin(), mode);
			else modes.push_back(mode);
		}
		++candidate.index;
	}
	return modes;
}

// The device's current format can be used as a last-resort mode only when the
// decoder can read it as one contiguous plane.
inline bool currentFormatUsable(const Node& node)
{
	return node.type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE || format(node.current).planes == 1;
}

// Device keys look like "snps_hdmirx (video1)". The node suffix changes across
// boots, the driver/card name does not.
inline std::string stableName(const std::string& key)
{
	const auto pos = key.rfind(" (video");
	return (pos != std::string::npos && !key.empty() && key.back() == ')') ? key.substr(0, pos) : key;
}

// Index of the device to use for a saved key after renumbering: an exact key
// match first, otherwise the only device with the same stable name. Returns -1
// when nothing or more than one device matches (caller falls back to auto).
inline int matchSavedDevice(const std::string& saved, const std::vector<std::string>& keys)
{
	for (size_t i = 0; i < keys.size(); ++i)
		if (keys[i] == saved) return int(i);
	const auto wanted = stableName(saved);
	int found = -1;
	for (size_t i = 0; i < keys.size(); ++i)
		if (stableName(keys[i]) == wanted)
		{
			if (found >= 0) return -1;
			found = int(i);
		}
	return found;
}
}
