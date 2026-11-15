#include <grabber/linux/v4l2/V4L2Capture.h>
#include <grabber/linux/v4l2/V4L2Supervisor.h>
#include <utils/Bgr24.h>
#include <functional>
#include <iostream>
#include <set>
#include <map>
#include <string>
#include <algorithm>
#include <cstdlib>

using namespace V4L2Capture;
#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error("check failed: " #condition); } while (false)
void rejects(const std::function<void()>& call)
{
	bool threw = false;
	try { call(); } catch (const std::runtime_error&) { threw = true; }
	REQUIRE(threw);
}

struct Fake : Operations
{
	unsigned long fail = 0;
	unsigned failMap = 0, maps = 0, unmaps = 0, releases = 0, queues = 0, off = 0;
	unsigned planeCount = 1, returnedCount = 4, index = 0, used = 24, offset = 0, mappingSize = 32;
	int dequeueError = 0;
	bool errorFlag = false, failUnmap = false, substituteFormat = false;
	std::set<void*> live;
	std::vector<unsigned long> calls;
	int control(int, unsigned long request, void* arg) override
	{
		calls.push_back(request);
		if (request == fail) { errno = EIO; return -1; }
		if (request == VIDIOC_REQBUFS)
		{
			auto& r = *static_cast<v4l2_requestbuffers*>(arg);
			REQUIRE(r.memory == V4L2_MEMORY_MMAP);
			if (!r.count) { ++releases; REQUIRE(live.empty()); }
			else r.count = returnedCount;
		}
		if (request == VIDIOC_TRY_FMT)
		{
			auto& p = static_cast<v4l2_format*>(arg)->fmt.pix_mp;
			p.num_planes = planeCount;
			p.plane_fmt[0].bytesperline = p.width * 3;
			p.plane_fmt[0].sizeimage = p.width * p.height * 3;
			if (substituteFormat) p.pixelformat = V4L2_PIX_FMT_NV12;
		}
		if (request == VIDIOC_QUERYBUF)
		{
			auto& b = *static_cast<v4l2_buffer*>(arg);
			if (b.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
			{
				REQUIRE(b.m.planes != nullptr && b.length == planeCount);
				for (unsigned p = 0; p < b.length; ++p) { b.m.planes[p].length = mappingSize; b.m.planes[p].m.mem_offset = (b.index * planeCount + p) * 4096; }
			}
			else { b.length = mappingSize; b.m.offset = b.index * 4096; }
		}
		if (request == VIDIOC_QBUF)
		{
			auto& b = *static_cast<v4l2_buffer*>(arg);
			if (b.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
			{
				REQUIRE(b.length == planeCount && b.m.planes != nullptr);
				for (unsigned p = 0; p < b.length; ++p) REQUIRE(b.m.planes[p].length == mappingSize);
			}
			++queues;
		}
		if (request == VIDIOC_DQBUF)
		{
			if (dequeueError) { errno = dequeueError; return -1; }
			auto& b = *static_cast<v4l2_buffer*>(arg); b.index = index;
			b.flags = errorFlag ? V4L2_BUF_FLAG_ERROR : 0;
			if (b.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
			{
				b.length = planeCount;
				for (unsigned p = 0; p < b.length; ++p) { b.m.planes[p].bytesused = used; b.m.planes[p].data_offset = offset; }
			}
			else b.bytesused = used;
		}
		if (request == VIDIOC_STREAMON || request == VIDIOC_STREAMOFF)
		{
			const auto t = *static_cast<v4l2_buf_type*>(arg);
			REQUIRE(t == V4L2_BUF_TYPE_VIDEO_CAPTURE || t == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
			if (request == VIDIOC_STREAMOFF) ++off;
		}
		return 0;
	}
	void* map(int, size_t size, off_t) override
	{
		if (++maps == failMap) { errno = ENOMEM; return MAP_FAILED; }
		auto* p = std::malloc(size); live.insert(p); return p;
	}
	int unmap(void* p, size_t) override
	{
		REQUIRE(live.erase(p) == 1); ++unmaps; std::free(p);
		return failUnmap ? -1 : 0;
	}
};

// A small simulated /dev: each node is a device with fixed capabilities.
struct SimDevice
{
	std::string card;
	__u32 caps = 0;
	std::vector<__u32> formats;   // ENUM_FMT order
	__u32 width = 640, height = 480, fourcc = V4L2_PIX_FMT_BGR24, planes = 1;
	bool hasPower = false; int power = 0;
	int rgbRange = -1;            // V4L2_CID_DV_RX_RGB_RANGE, -1 = not exposed
	int dvErrno = ENOTTY;         // 0 = timings detected
	unsigned events = 0, dqevents = 0;
};

struct SimSystem : Operations
{
	std::map<std::string, SimDevice*> nodes;
	std::map<int, SimDevice*> open_;
	int nextFd = 10;
	unsigned opens = 0, closes = 0;

	int open(const char* path) override
	{
		auto it = nodes.find(path);
		if (it == nodes.end()) { errno = ENOENT; return -1; }
		++opens; open_[nextFd] = it->second; return nextFd++;
	}
	int close(int fd) override { ++closes; return open_.erase(fd) ? 0 : -1; }
	int control(int fd, unsigned long request, void* arg) override
	{
		auto it = open_.find(fd);
		if (it == open_.end()) { errno = EBADF; return -1; }
		SimDevice& d = *it->second;
		const bool mplane = d.caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE;
		switch (request)
		{
			case VIDIOC_QUERYCAP:
			{
				auto& c = *static_cast<v4l2_capability*>(arg);
				c = {}; std::snprintf(reinterpret_cast<char*>(c.card), sizeof(c.card), "%s", d.card.c_str());
				c.capabilities = d.caps | V4L2_CAP_DEVICE_CAPS; c.device_caps = d.caps;
				return 0;
			}
			case VIDIOC_G_FMT:
			{
				auto& f = *static_cast<v4l2_format*>(arg);
				if (!(d.caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE))) { errno = EINVAL; return -1; }
				requestFormat(f, d.width, d.height, d.fourcc);
				if (mplane) { f.fmt.pix_mp.num_planes = d.planes; f.fmt.pix_mp.plane_fmt[0].bytesperline = d.width * 3; f.fmt.pix_mp.plane_fmt[0].sizeimage = d.width * d.height * 3; }
				else { f.fmt.pix.bytesperline = d.width * 3; f.fmt.pix.sizeimage = d.width * d.height * 3; }
				return 0;
			}
			case VIDIOC_ENUM_FMT:
			{
				auto& e = *static_cast<v4l2_fmtdesc*>(arg);
				if (e.index >= d.formats.size()) { errno = EINVAL; return -1; }
				e.pixelformat = d.formats[e.index]; return 0;
			}
			case VIDIOC_TRY_FMT:
			{
				auto& p = static_cast<v4l2_format*>(arg)->fmt.pix_mp;
				if (std::find(d.formats.begin(), d.formats.end(), p.pixelformat) == d.formats.end()) { errno = EINVAL; return -1; }
				p.width = d.width; p.height = d.height; p.num_planes = d.planes;
				p.plane_fmt[0].bytesperline = d.width * 3; p.plane_fmt[0].sizeimage = d.width * d.height * 3;
				return 0;
			}
			case VIDIOC_G_CTRL:
			{
				auto& c = *static_cast<v4l2_control*>(arg);
				if (c.id == V4L2_CID_DV_RX_RGB_RANGE && d.rgbRange >= 0) { c.value = d.rgbRange; return 0; }
				if (c.id != V4L2_CID_DV_RX_POWER_PRESENT || !d.hasPower) { errno = EINVAL; return -1; }
				c.value = d.power; return 0;
			}
			case VIDIOC_QUERY_DV_TIMINGS:
			{
				if (d.dvErrno) { errno = d.dvErrno; return -1; }
				auto& t = *static_cast<v4l2_dv_timings*>(arg);
				t = {}; t.type = V4L2_DV_BT_656_1120; t.bt.width = 1920; t.bt.height = 1080; return 0;
			}
			case VIDIOC_DQEVENT:
				if (!d.events) { errno = ENOENT; return -1; }
				--d.events; ++d.dqevents; return 0;
			default:
				errno = ENOTTY; return -1;
		}
	}
};

// The Orange Pi 5 Plus node set seen on Armbian 6.18 (mainline), plus a USB grabber.
struct Board
{
	SimDevice hdmirx, uvc, rkvdec, hantro, rga, isp, output;
	Board()
	{
		hdmirx.card = "snps_hdmirx"; hdmirx.caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING;
		hdmirx.formats = { V4L2_PIX_FMT_BGR24 }; hdmirx.hasPower = true; hdmirx.power = 0; hdmirx.dvErrno = ENOLINK;
		uvc.card = "USB Video"; uvc.caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
		uvc.formats = { V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV }; uvc.fourcc = V4L2_PIX_FMT_YUYV;
		rkvdec.card = "rkvdec"; rkvdec.caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING; rkvdec.formats = { V4L2_PIX_FMT_NV12 };
		// Older drivers advertise both queues as well as the M2M bit.
		hantro.card = "hantro-vpu"; hantro.caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_OUTPUT_MPLANE | V4L2_CAP_STREAMING;
		hantro.formats = { V4L2_PIX_FMT_NV12 };
		rga.card = "rockchip-rga"; rga.caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING; rga.formats = { V4L2_PIX_FMT_BGR24 };
		isp.card = "rkisp-stats"; isp.caps = V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING;
		output.card = "rkisp-params"; output.caps = V4L2_CAP_VIDEO_OUTPUT_MPLANE | V4L2_CAP_STREAMING;
	}
	std::vector<SimDevice*> all() { return { &hdmirx, &uvc, &rkvdec, &hantro, &rga, &isp, &output }; }
};

// Mirrors the grabber's discovery loop: every /dev/videoN, classified only
// by what the node reports.
std::vector<std::pair<std::string, Node>> discover(SimSystem& sys)
{
	std::vector<std::pair<std::string, Node>> found;
	for (const auto& entry : sys.nodes)
	{
		const int fd = sys.open(entry.first.c_str());
		REQUIRE(fd >= 0);
		Node node; std::string reason;
		if (probeNode(sys, fd, node, reason)) found.push_back({ entry.second->card, node });
		else REQUIRE(!reason.empty());
		sys.close(fd);
	}
	return found;
}

// Records what the supervisor asks for and checks resource ownership after
// every call: capture, retry timer and signal wait are never held together,
// and nothing is held after stop().
struct FakeHost : SupervisorHost
{
	enum class Next { Ok, NoSignal, InitFail, StreamFail, Generic };
	std::vector<Next> script;            // results of successive startCapture calls
	Supervisor* sup = nullptr;
	bool capture = false, waitOpen = false, polling = false;
	int retryMs = -1;                    // -1 = no retry armed
	bool nodeOpens = true;
	std::string node = "/dev/video1";
	SignalStatus waiting{ Signal::Absent, "no source detected (power_present=0)" };
	unsigned starts = 0, stops = 0, opens = 0, probes = 0;
	std::vector<bool> retryingDuringStart;
	std::vector<std::pair<LogLevel, std::string>> logs;

	void invariant() const
	{
		REQUIRE(int(capture) + int(retryMs >= 0) + int(polling) <= 1);
		REQUIRE(!waitOpen || polling);
	}
	void startCapture() override
	{
		REQUIRE(!capture && !polling && retryMs < 0);   // clean slate before every start
		++starts;
		retryingDuringStart.push_back(sup->retrying());
		const Next n = script.empty() ? Next::Ok : script.front();
		if (!script.empty()) script.erase(script.begin());
		capture = true;                  // partially acquired before failing
		switch (n)
		{
			case Next::Ok: return;
			case Next::NoSignal: throw NoSignal("no source detected (power_present=0)");
			case Next::InitFail: throw CaptureError(Failure::Initialization, "format negotiation: VIDIOC_S_FMT: Device or resource busy");
			case Next::StreamFail: throw CaptureError(Failure::Streaming, "streaming: VIDIOC_STREAMON: Input/output error");
			case Next::Generic: throw std::runtime_error("buffer setup: mmap: Cannot allocate memory");
		}
	}
	void stopCapture() override { ++stops; capture = false; }
	void armRetry(int ms) override { REQUIRE(ms > 0); retryMs = ms; }
	void cancelRetry() override { retryMs = -1; }
	bool openSignalWait(const std::string& n) override
	{
		REQUIRE(!capture && retryMs < 0 && !polling);
		++opens; polling = true;
		waitOpen = nodeOpens && !n.empty();
		return waitOpen;
	}
	void closeSignalWait() override { waitOpen = false; polling = false; }
	SignalStatus probeWaitingSignal() override { REQUIRE(waitOpen); ++probes; return waiting; }
	std::string activeNode() override { return node; }
	void log(LogLevel level, const std::string& m) override { logs.push_back({ level, m }); }

	unsigned count(LogLevel level, const std::string& needle) const
	{
		unsigned n = 0;
		for (const auto& l : logs) n += l.first == level && l.second.find(needle) != std::string::npos;
		return n;
	}
};

struct Rig
{
	FakeHost host;
	Supervisor sup{ host };
	Rig() { host.sup = &sup; }
	template<typename F> void step(F&& f) { f(); host.invariant(); }
};

int main()
{
	unsigned passed = 0;
	auto test = [&](const char* name, const std::function<void()>& fn) {
		try { fn(); ++passed; std::cout << "PASS " << name << '\n'; }
		catch (const std::exception& e) { std::cerr << "FAIL " << name << ": " << e.what() << '\n'; std::exit(1); }
	};
	test("capability selection and DEVICE_CAPS", [] {
		v4l2_capability c{}; c.capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
		REQUIRE(captureType(c) == V4L2_BUF_TYPE_VIDEO_CAPTURE);
		c.capabilities = V4L2_CAP_DEVICE_CAPS | V4L2_CAP_VIDEO_CAPTURE;
		c.device_caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING;
		REQUIRE(captureType(c) == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		c.device_caps |= V4L2_CAP_VIDEO_CAPTURE;
		REQUIRE(captureType(c) == V4L2_BUF_TYPE_VIDEO_CAPTURE);
		c.device_caps = V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING; rejects([&] { captureType(c); });
		c.device_caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE; rejects([&] { captureType(c); });
	});
	test("recovery backoff, source-change fast path and quiet repeats", [] {
		Recovery r;
		REQUIRE(r.nextDelay(false) == 3000 && r.nextDelay(false) == 6000);
		REQUIRE(r.nextDelay(false) == 12000 && r.nextDelay(false) == 24000);
		REQUIRE(r.nextDelay(false) == 30000 && r.nextDelay(false) == 30000);
		REQUIRE(r.failures() == 6);
		// A source change retries quickly and restarts the backoff ladder.
		REQUIRE(r.nextDelay(true) == Recovery::sourceChangeDelayMs);
		REQUIRE(r.nextDelay(false) == 3000);
		REQUIRE(r.shouldWarn("no frames") && !r.shouldWarn("no frames") && !r.shouldWarn("no frames"));
		REQUIRE(r.shouldWarn("source changed") && r.shouldWarn("no frames"));
		REQUIRE(r.frameReceived() == 8 && r.failures() == 0 && r.frameReceived() == 0);
		REQUIRE(r.shouldWarn("no frames"));
		REQUIRE(r.nextDelay(false) == 3000);
	});
	test("isolated short frames are dropped, a run restarts", [] {
		Recovery r;
		REQUIRE(!r.shortFrame() && !r.shortFrame());
		r.frameReceived();
		REQUIRE(!r.shortFrame() && !r.shortFrame() && r.shortFrame());
	});
	test("BGR24 format union, negotiated stride and rejected formats", [] {
		Fake f; v4l2_format fmt{}; fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		requestFormat(fmt, 2, 2, V4L2_PIX_FMT_BGR24);
		fmt.fmt.pix_mp.num_planes = 1; fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 8;
		fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 16; fmt.fmt.pix_mp.quantization = V4L2_QUANTIZATION_LIM_RANGE;
		auto result = negotiate(f, 3, fmt);
		REQUIRE(result.fourcc == V4L2_PIX_FMT_BGR24 && result.stride == 8 && result.width == 2 && result.planes == 1);
		REQUIRE(result.quantization == V4L2_QUANTIZATION_LIM_RANGE);
		f.fail = VIDIOC_S_FMT; rejects([&] { negotiate(f, 3, fmt); }); f.fail = 0;
		fmt.fmt.pix_mp.num_planes = 2; rejects([&] { negotiate(f, 3, fmt); });
		fmt.fmt.pix_mp.num_planes = 0; rejects([&] { negotiate(f, 3, fmt); });
		fmt.fmt.pix_mp.num_planes = 1; fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 5;
		rejects([&] { negotiate(f, 3, fmt); });
		fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; requestFormat(fmt, 2, 2, V4L2_PIX_FMT_BGR24);
		fmt.fmt.pix.bytesperline = 6; fmt.fmt.pix.sizeimage = 12;
		REQUIRE(negotiate(f, 3, fmt).planes == 1);
	});
	test("advertised modes require successful, matching TRY_FMT", [] {
		Fake f; const auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		REQUIRE(tryFormat(f, 3, type, 1920, 1080, V4L2_PIX_FMT_BGR24));
		f.fail = VIDIOC_TRY_FMT; REQUIRE(!tryFormat(f, 3, type, 1920, 1080, V4L2_PIX_FMT_NV12));
		f.fail = 0; f.substituteFormat = true;
		REQUIRE(!tryFormat(f, 3, type, 1920, 1080, V4L2_PIX_FMT_BGR24));
		f.substituteFormat = false; f.planeCount = 2;
		REQUIRE(!tryFormat(f, 3, type, 1920, 1080, V4L2_PIX_FMT_BGR24));
	});
	test("driver/bus identity survives node renumbering", [] {
		v4l2_capability cap{};
		std::strcpy(reinterpret_cast<char*>(cap.driver), "snps_hdmirx");
		std::strcpy(reinterpret_cast<char*>(cap.bus_info), "platform:fdee0000.hdmi_receiver");
		const auto id = deviceIdentity(cap);
		REQUIRE(id == "v4l2:snps_hdmirx@platform:fdee0000.hdmi_receiver");
		std::strcpy(reinterpret_cast<char*>(cap.card), "stream_hdmirx (video2)");
		REQUIRE(deviceIdentity(cap) == id);
		std::strcpy(reinterpret_cast<char*>(cap.card), "stream_hdmirx (video7)");
		REQUIRE(deviceIdentity(cap) == id && isDeviceIdentity(id));
		REQUIRE(!isDeviceIdentity("/dev/video2"));
		cap.bus_info[0] = 0; REQUIRE(deviceIdentity(cap).empty());
		std::memset(cap.driver, 'x', sizeof(cap.driver));
		std::memset(cap.bus_info, 'y', sizeof(cap.bus_info));
		REQUIRE(deviceIdentity(cap).size() == 5 + sizeof(cap.driver) + 1 + sizeof(cap.bus_info));
	});
	test("mainline DV acquisition and canonical timing metadata", [] {
		struct TimingDriver : Operations {
			v4l2_dv_timings detected{}, current{};
			unsigned sets = 0;
			int queryError = 0, setError = 0;
			TimingDriver() { detected.type = V4L2_DV_BT_656_1120; detected.bt.width = 1920;
				detected.bt.height = 1080; detected.bt.pixelclock = 148500000; current = detected; }
			int control(int, unsigned long request, void* arg) override {
				if (request == VIDIOC_QUERY_DV_TIMINGS) {
					if (queryError) { errno = queryError; return -1; }
					*static_cast<v4l2_dv_timings*>(arg) = detected;
				} else if (request == VIDIOC_G_DV_TIMINGS) *static_cast<v4l2_dv_timings*>(arg) = current;
				else if (request == VIDIOC_S_DV_TIMINGS) {
					++sets; if (setError) { errno = setError; return -1; }
					current = *static_cast<v4l2_dv_timings*>(arg);
				}
				return 0;
			}
		} f;
		f.current.bt.flags = V4L2_DV_FL_CAN_REDUCE_FPS;
		f.current.bt.reserved[0] = 1;
		refreshTimings(f, 3); REQUIRE(f.sets == 0);
		f.detected.bt.width = 1280; f.detected.bt.height = 720;
		refreshTimings(f, 3); REQUIRE(f.sets == 1 && f.current.bt.width == 1280);
		for (auto error : {ENOLINK, ENOLCK, EAGAIN, ERANGE}) {
			f.queryError = error; rejects([&] { refreshTimings(f, 3); });
		}
		for (auto error : {ENOTTY, EINVAL}) { f.queryError = error; refreshTimings(f, 3); }
		f.queryError = 0; f.detected.bt.pixelclock = 74250000; f.setError = EBUSY;
		rejects([&] { refreshTimings(f, 3); });
		f.setError = 0; refreshTimings(f, 3); REQUIRE(f.current.bt.pixelclock == 74250000);
	});
	test("verified 1080p BGR3, padded stride and negotiated map size", [] {
		v4l2_format fmt{}; fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		auto& p = fmt.fmt.pix_mp;
		p.width = 1920; p.height = 1080; p.pixelformat = V4L2_PIX_FMT_BGR24;
		p.num_planes = 1; p.plane_fmt[0].bytesperline = 5760; p.plane_fmt[0].sizeimage = 6220800;
		p.quantization = V4L2_QUANTIZATION_DEFAULT;
		const auto native = format(fmt); validateFormat(native);
		REQUIRE(native.stride == 5760 && native.size == 6220800 && native.planes == 1);
		REQUIRE(native.quantization == V4L2_QUANTIZATION_DEFAULT);
		p.plane_fmt[0].bytesperline = 5824; p.plane_fmt[0].sizeimage = 5824 * 1080;
		validateFormat(format(fmt)); REQUIRE(format(fmt).stride == 5824);
		Fake f; Buffers b(f);
		rejects([&] { b.allocate(3, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 1, native.size); });
		REQUIRE(f.live.empty() && f.releases == 1 && f.maps == 0);
		f.mappingSize = native.size; f.used = native.size;
		b.allocate(3, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 1, native.size);
		b.start(); Descriptor d(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 1);
		REQUIRE(b.dequeue(d)); REQUIRE(d.planes[0].bytesused == 6220800);
		REQUIRE(b.mappings[d.buffer.index][0].size == native.size); REQUIRE(b.clear());
	});
	for (auto type : {V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE})
	{
		test(type == V4L2_BUF_TYPE_VIDEO_CAPTURE ? "single-planar lifecycle regression" : "one-plane MPLANE lifecycle", [=] {
			Fake f; { Buffers b(f); b.allocate(3, type, 1); b.start(); Descriptor d(type, 1);
			REQUIRE(b.dequeue(d)); REQUIRE(d.buffer.index == 0); b.queue(d.buffer.index);
			REQUIRE(f.queues == 5); REQUIRE(b.clear()); REQUIRE(b.clear()); }
			REQUIRE(f.maps == 4 && f.unmaps == 4 && f.off == 1 && f.releases == 1 && f.live.empty());
		});
	}
	test("multi-plane mapping cleanup and partial allocation rollback", [] {
		for (unsigned failAt : {0u, 1u, 3u, 8u})
		{
			Fake f; f.planeCount = 2; f.failMap = failAt;
			{ Buffers b(f); if (failAt) rejects([&] { b.allocate(3, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 2); });
			else { b.allocate(3, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 2); b.start(); } }
			REQUIRE(f.live.empty() && f.releases == 1);
			REQUIRE(f.unmaps == (failAt ? failAt - 1 : 8));
		}
	});
	test("REQBUFS and QUERYBUF failures and insufficient memory", [] {
		for (auto request : {VIDIOC_REQBUFS, VIDIOC_QUERYBUF})
		{
			Fake f; f.fail = request; Buffers b(f);
			rejects([&] { b.allocate(3, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 1); });
			REQUIRE(f.live.empty()); REQUIRE(f.releases == (request == VIDIOC_QUERYBUF ? 1u : 0u));
		}
		Fake f; f.returnedCount = 1; Buffers b(f);
		rejects([&] { b.allocate(3, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1); }); REQUIRE(f.releases == 1);
	});
	test("stream failures, EAGAIN, invalid payloads and restart", [] {
		Fake f; Buffers b(f); const auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.allocate(3, type, 1); f.fail = VIDIOC_QBUF; rejects([&] { b.start(); });
		f.fail = VIDIOC_STREAMON; rejects([&] { b.start(); }); f.fail = 0; b.start();
		Descriptor d(type, 1); f.dequeueError = EAGAIN; REQUIRE(!b.dequeue(d));
		f.dequeueError = EIO; rejects([&] { b.dequeue(d); }); f.dequeueError = 0;
		f.index = 99; rejects([&] { b.dequeue(d); }); f.index = 0;
		f.used = 33; rejects([&] { b.dequeue(d); }); f.used = 24;
		f.offset = 25; rejects([&] { b.dequeue(d); }); f.offset = 4;
		REQUIRE(b.dequeue(d)); REQUIRE(d.planes[0].bytesused - d.planes[0].data_offset == 20);
		b.clear(); b.allocate(3, type, 1); b.start(); REQUIRE(b.dequeue(d)); b.clear();
		REQUIRE(f.live.empty() && f.releases == 2 && f.off == 2);
	});
	test("cleanup continues after failed STREAMOFF and unmap", [] {
		Fake f; Buffers b(f); b.allocate(3, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 1); b.start();
		f.fail = VIDIOC_STREAMOFF; f.failUnmap = true; REQUIRE(!b.clear());
		REQUIRE(f.unmaps == 4 && f.releases == 1 && f.live.empty());
	});
	test("BGR channel order, limited range and LUT indexing", [] {
		uint8_t source[3] = {10, 20, 230}, dest[3]{};
		Bgr24::pixel<false>(source, dest, false, nullptr);
		REQUIRE(dest[0] == 230 && dest[1] == 20 && dest[2] == 10);
		source[0] = 16; source[1] = 126; source[2] = 235;
		Bgr24::pixel<false>(source, dest, true, nullptr);
		REQUIRE(dest[0] == 255 && dest[1] == 128 && dest[2] == 0);
		REQUIRE(Bgr24::expandLimited(0) == 0 && Bgr24::expandLimited(255) == 255);
		std::vector<uint8_t> lut(256 * 256 * 256 * 3);
		const auto index = (255u | (128u << 8)) * 3; lut[index] = 7; lut[index + 1] = 8; lut[index + 2] = 9;
		Bgr24::pixel<true>(source, dest, true, lut.data());
		REQUIRE(dest[0] == 7 && dest[1] == 8 && dest[2] == 9);
	});
	test("limited-range table matches reference and row conversion", [] {
		for (int v = 0; v < 256; ++v)
			REQUIRE(Bgr24::limitedTable[v] == Bgr24::expandLimited(uint8_t(v)));
		// Two pixels, quarter step: reads pixels 0 and 2 only, writes six bytes.
		const uint8_t source[12] = {16, 126, 235, 1, 2, 3, 0, 255, 128, 4, 5, 6};
		uint8_t dest[6]{};
		Bgr24::row<false, true, 2>(source, dest, 2, nullptr);
		REQUIRE(dest[0] == 255 && dest[1] == 128 && dest[2] == 0);
		REQUIRE(dest[3] == Bgr24::expandLimited(128) && dest[4] == 255 && dest[5] == 0);
		Bgr24::row<false, false, 1>(source, dest, 2, nullptr);
		REQUIRE(dest[0] == 235 && dest[1] == 126 && dest[2] == 16 && dest[3] == 3 && dest[4] == 2 && dest[5] == 1);
	});
	test("discovery is independent of /dev/videoN ordering", [] {
		Board board; auto devices = board.all();
		for (size_t rotation = 0; rotation < devices.size(); ++rotation)
		{
			SimSystem sys;
			std::rotate(devices.begin(), devices.begin() + 1, devices.end());
			for (size_t i = 0; i < devices.size(); ++i) sys.nodes["/dev/video" + std::to_string(i)] = devices[i];
			const auto found = discover(sys);
			REQUIRE(found.size() == 2);
			std::set<std::string> cards;
			for (const auto& f : found) cards.insert(f.first);
			REQUIRE(cards == (std::set<std::string>{ "snps_hdmirx", "USB Video" }));
			REQUIRE(sys.opens == sys.closes && sys.open_.empty());
		}
	});
	test("codec, RGA, metadata and output nodes are ignored", [] {
		Board board; SimSystem sys;
		for (SimDevice* d : { &board.rkvdec, &board.hantro, &board.rga, &board.isp, &board.output })
		{
			sys.nodes = { { "/dev/video9", d } };
			const int fd = sys.open("/dev/video9");
			Node node; std::string reason;
			REQUIRE(!probeNode(sys, fd, node, reason));
			REQUIRE(reason.rfind("discovery:", 0) == 0);
			sys.close(fd);
		}
		v4l2_capability c{}; c.capabilities = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING;
		rejects([&] { captureType(c); });
	});
	test("MPLANE receiver recognized, one plane accepted, BGR24 preferred", [] {
		Board board; SimSystem sys; sys.nodes = { { "/dev/video1", &board.hdmirx } };
		board.hdmirx.formats = { V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV16, V4L2_PIX_FMT_BGR24 };
		const int fd = sys.open("/dev/video1");
		Node node; std::string reason;
		REQUIRE(probeNode(sys, fd, node, reason));
		REQUIRE(node.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && currentFormatUsable(node));
		const auto modes = currentTimingModes(sys, fd, node, [](__u32 f) { return f != V4L2_PIX_FMT_NV16; });
		REQUIRE(modes.size() == 2 && modes[0].fourcc == V4L2_PIX_FMT_BGR24 && modes[1].fourcc == V4L2_PIX_FMT_NV12);
		REQUIRE(modes[0].width == 640 && modes[0].height == 480);
		sys.close(fd);
	});
	test("multi-memory-plane formats are not offered or negotiated", [] {
		Board board; SimSystem sys; sys.nodes = { { "/dev/video1", &board.hdmirx } };
		board.hdmirx.planes = 2; board.hdmirx.formats = { V4L2_PIX_FMT_NV12M };
		const int fd = sys.open("/dev/video1");
		Node node; std::string reason;
		REQUIRE(probeNode(sys, fd, node, reason));
		REQUIRE(!currentFormatUsable(node));
		REQUIRE(currentTimingModes(sys, fd, node, [](__u32) { return true; }).empty());
		v4l2_format fmt = node.current; rejects([&] { validateFormat(format(fmt)); });
		sys.close(fd);
		// Single-planar devices are unaffected by the plane rule.
		Node uvc; uvc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; REQUIRE(currentFormatUsable(uvc));
		REQUIRE(currentTimingModes(sys, -1, uvc, [](__u32) { return true; }).empty());
	});
	test("absent HDMI signal does not prevent receiver discovery", [] {
		Board board; SimSystem sys; sys.nodes = { { "/dev/video1", &board.hdmirx } };
		const int fd = sys.open("/dev/video1");
		Node node; std::string reason;
		REQUIRE(probeSignal(sys, fd).state == Signal::Absent);
		REQUIRE(probeNode(sys, fd, node, reason));
		REQUIRE(currentTimingModes(sys, fd, node, [](__u32) { return true; }).size() == 1);
		sys.close(fd);
	});
	test("saved device follows its stable name across renumbering", [] {
		REQUIRE(stableName("snps_hdmirx (video2)") == "snps_hdmirx" && stableName("plain") == "plain");
		REQUIRE(stableName("odd (video1") == "odd (video1");
		const std::vector<std::string> now = { "USB Video (video0)", "snps_hdmirx (video1)" };
		REQUIRE(matchSavedDevice("snps_hdmirx (video5)", now) == 1);
		REQUIRE(matchSavedDevice("USB Video (video0)", now) == 0);
		REQUIRE(matchSavedDevice("missing (video3)", now) == -1);
		// Exact match wins; two identical cards are ambiguous after renumbering.
		const std::vector<std::string> twins = { "USB Video (video0)", "USB Video (video2)" };
		REQUIRE(matchSavedDevice("USB Video (video2)", twins) == 1);
		REQUIRE(matchSavedDevice("USB Video (video4)", twins) == -1);
	});
	test("signal probe: power_present, DV timings and unsupported devices", [] {
		Board board; SimSystem sys; sys.nodes = { { "/dev/video1", &board.hdmirx }, { "/dev/video0", &board.uvc } };
		const int rx = sys.open("/dev/video1");
		auto s = probeSignal(sys, rx);
		REQUIRE(s.state == Signal::Absent && s.detail.find("power_present=0") != std::string::npos);
		board.hdmirx.power = 1;   // cable in, source not yet transmitting
		s = probeSignal(sys, rx);
		REQUIRE(s.state == Signal::Absent && s.detail.find("ENOLINK") != std::string::npos);
		board.hdmirx.dvErrno = ENOLCK; REQUIRE(probeSignal(sys, rx).state == Signal::Absent);
		board.hdmirx.dvErrno = ERANGE; REQUIRE(probeSignal(sys, rx).detail.find("out of range") != std::string::npos);
		board.hdmirx.dvErrno = 0;
		s = probeSignal(sys, rx);
		REQUIRE(s.state == Signal::Present && s.detail == "detected 1920x1080p");
		// Power control only, no DV timings: not enough to claim a signal.
		board.hdmirx.dvErrno = ENOTTY; REQUIRE(probeSignal(sys, rx).state == Signal::Unknown);
		board.hdmirx.hasPower = false; board.hdmirx.dvErrno = ENOLINK;
		REQUIRE(probeSignal(sys, rx).state == Signal::Absent);
		// USB/UVC: neither interface exists, behaviour stays unchanged.
		const int usb = sys.open("/dev/video0");
		s = probeSignal(sys, usb);
		REQUIRE(s.state == Signal::Unknown && s.detail.empty());
		sys.close(rx); sys.close(usb);
	});
	test("failure categories are distinct in logs", [] {
		std::set<std::string> labels;
		for (auto f : { Failure::NoSignal, Failure::Timeout, Failure::Initialization, Failure::Streaming, Failure::SourceChange })
			labels.insert(label(f));
		REQUIRE(labels.size() == 5);
		REQUIRE(describe(Failure::Streaming, "streaming: VIDIOC_DQBUF: EIO") == "streaming failed: VIDIOC_DQBUF: EIO");
		REQUIRE(describe(Failure::Initialization, "format negotiation: VIDIOC_S_FMT: EBUSY") ==
			"initialization failed: format negotiation: VIDIOC_S_FMT: EBUSY");
		REQUIRE(describe(Failure::NoSignal, "no source detected (power_present=0)").rfind("no signal: ", 0) == 0);
		REQUIRE(describe(Failure::Timeout, "no frames for five seconds") == "timeout: no frames for five seconds");
		bool caught = false;
		try { throw NoSignal("x"); } catch (const std::runtime_error& e) { caught = std::string(e.what()) == "x"; }
		REQUIRE(caught);
		REQUIRE(Recovery::signalPollMs > 0 && Recovery::signalPollMs <= Recovery::initialDelayMs);
	});
	test("RGB range evidence: control, names and mismatch reporting", [] {
		Board board; SimSystem sys; sys.nodes = { { "/dev/video1", &board.hdmirx }, { "/dev/video0", &board.uvc } };
		const int rx = sys.open("/dev/video1"), usb = sys.open("/dev/video0");
		REQUIRE(receiverRgbRange(sys, usb) == RgbRange::Unsupported && std::string(name(RgbRange::Unsupported)) == "not exposed");
		REQUIRE(receiverRgbRange(sys, rx) == RgbRange::Unsupported);
		board.hdmirx.rgbRange = V4L2_DV_RGB_RANGE_AUTO; REQUIRE(receiverRgbRange(sys, rx) == RgbRange::Auto);
		board.hdmirx.rgbRange = V4L2_DV_RGB_RANGE_LIMITED; REQUIRE(receiverRgbRange(sys, rx) == RgbRange::Limited);
		board.hdmirx.rgbRange = V4L2_DV_RGB_RANGE_FULL; REQUIRE(receiverRgbRange(sys, rx) == RgbRange::Full);
		board.hdmirx.rgbRange = 7; REQUIRE(receiverRgbRange(sys, rx) == RgbRange::Auto);
		REQUIRE(std::string(quantizationName(V4L2_QUANTIZATION_DEFAULT)) == "default" &&
			std::string(quantizationName(V4L2_QUANTIZATION_LIM_RANGE)) == "limited" &&
			std::string(quantizationName(V4L2_QUANTIZATION_FULL_RANGE)) == "full" && std::string(quantizationName(99)) == "unknown");
		// Agreement, or a control without a range, is silent.
		for (auto q : { V4L2_QUANTIZATION_DEFAULT, V4L2_QUANTIZATION_FULL_RANGE, V4L2_QUANTIZATION_LIM_RANGE })
			for (auto r : { RgbRange::Unsupported, RgbRange::Auto })
				REQUIRE(rangeMismatch(q, r).empty());
		REQUIRE(rangeMismatch(V4L2_QUANTIZATION_LIM_RANGE, RgbRange::Limited).empty());
		REQUIRE(rangeMismatch(V4L2_QUANTIZATION_FULL_RANGE, RgbRange::Full).empty());
		REQUIRE(rangeMismatch(V4L2_QUANTIZATION_DEFAULT, RgbRange::Full).empty());
		// Disagreements name what the decoder will do.
		REQUIRE(rangeMismatch(V4L2_QUANTIZATION_DEFAULT, RgbRange::Limited).find("not expanded") != std::string::npos);
		REQUIRE(rangeMismatch(V4L2_QUANTIZATION_FULL_RANGE, RgbRange::Limited).find("quantization is full") != std::string::npos);
		REQUIRE(rangeMismatch(V4L2_QUANTIZATION_LIM_RANGE, RgbRange::Full).find("clip") != std::string::npos);
		sys.close(rx); sys.close(usb);
	});
	test("supervisor: no signal at start waits on an event handle, then starts", [] {
		Rig r; r.host.script = { FakeHost::Next::NoSignal };
		r.step([&] { REQUIRE(!r.sup.start()); });
		REQUIRE(r.sup.state() == Supervisor::State::WaitingForSignal);
		REQUIRE(r.host.waitOpen && !r.host.capture && r.host.retryMs < 0 && r.host.opens == 1);
		REQUIRE(r.host.count(LogLevel::Info, "no signal: no source detected (power_present=0) on /dev/video1; capture paused") == 1);
		// Many polls while dark: one Info line, no start, no retry timer, no reopen.
		for (int i = 0; i < 50; ++i) r.step([&] { r.sup.signalPollFired(); });
		REQUIRE(r.host.starts == 1 && r.host.probes == 50 && r.host.opens == 1);
		REQUIRE(r.host.count(LogLevel::Info, "no signal:") == 1);
		// Signal returns: the very next poll or event starts capture as a recovery.
		r.host.waiting = { Signal::Present, "detected 1920x1080p" };
		r.step([&] { r.sup.signalPollFired(); });
		REQUIRE(r.sup.state() == Supervisor::State::Streaming && r.host.capture && !r.host.polling);
		REQUIRE(r.host.starts == 2 && r.host.retryingDuringStart == (std::vector<bool>{ false, true }));
		REQUIRE(r.host.count(LogLevel::Info, "Input signal on /dev/video1 (detected 1920x1080p); starting capture") == 1);
		REQUIRE(!r.sup.retrying());
	});
	test("supervisor: signal lost while streaming pauses instead of timing out", [] {
		Rig r;
		r.step([&] { REQUIRE(r.sup.start()); });
		r.step([&] { r.sup.stalled({ Signal::Absent, "no signal (QUERY_DV_TIMINGS: ENOLINK)" }); });
		REQUIRE(r.sup.state() == Supervisor::State::WaitingForSignal && r.host.retryMs < 0 && r.host.stops >= 1);
		REQUIRE(r.host.count(LogLevel::Warning, "timeout") == 0);
		// Same through the source-change event on the streaming fd.
		Rig e;
		e.step([&] { e.sup.start(); });
		e.step([&] { e.sup.sourceEvent(true, { Signal::Absent, "no source detected (power_present=0)" }); });
		REQUIRE(e.sup.state() == Supervisor::State::WaitingForSignal);
	});
	test("supervisor: USB-style failures keep the capped backoff and fast source change", [] {
		Rig r;
		r.host.script = { FakeHost::Next::StreamFail, FakeHost::Next::StreamFail, FakeHost::Next::StreamFail,
			FakeHost::Next::StreamFail, FakeHost::Next::StreamFail, FakeHost::Next::StreamFail };
		std::vector<int> delays;
		r.step([&] { r.sup.start(); }); delays.push_back(r.host.retryMs);
		for (int i = 0; i < 5; ++i) { r.step([&] { r.sup.retryTimerFired(); }); delays.push_back(r.host.retryMs); }
		REQUIRE(delays == (std::vector<int>{ 3000, 6000, 12000, 24000, 30000, 30000 }));
		REQUIRE(r.host.count(LogLevel::Warning, "streaming failed: VIDIOC_STREAMON") == 1);
		REQUIRE(r.host.retryingDuringStart.front() == false && r.host.retryingDuringStart.back() == true);
		REQUIRE(r.host.opens == 0);    // never enters the signal wait without a reported absence
		r.step([&] { r.sup.retryTimerFired(); });   // script exhausted: succeeds
		REQUIRE(r.sup.state() == Supervisor::State::Streaming);
		r.step([&] { r.sup.frameReceived(); });
		REQUIRE(r.host.count(LogLevel::Info, "Capture recovered after 6 failed attempt(s)") == 1);
		r.step([&] { r.sup.sourceEvent(true, {}); });
		REQUIRE(r.sup.state() == Supervisor::State::Backoff && r.host.retryMs == Recovery::sourceChangeDelayMs);
		REQUIRE(r.host.count(LogLevel::Info, "source changed: input timing or format changed; restarting capture in 500 ms") == 1);
	});
	test("supervisor: failure categories from start and streaming", [] {
		Rig r; r.host.script = { FakeHost::Next::InitFail, FakeHost::Next::Generic };
		r.step([&] { r.sup.start(); });
		REQUIRE(r.host.count(LogLevel::Warning, "initialization failed: format negotiation") == 1);
		r.step([&] { r.sup.retryTimerFired(); });
		REQUIRE(r.host.count(LogLevel::Warning, "initialization failed: buffer setup: mmap") == 1);
		r.step([&] { r.sup.retryTimerFired(); });
		r.step([&] { r.sup.stalled({ Signal::Present, "detected 1920x1080p" }); });
		REQUIRE(r.host.count(LogLevel::Warning, "timeout: no frames for five seconds (receiver: detected 1920x1080p)") == 1);
		r.step([&] { r.sup.retryTimerFired(); });
		r.step([&] { r.sup.streamFailed("streaming: VIDIOC_DQBUF: Input/output error"); });
		REQUIRE(r.host.count(LogLevel::Warning, "streaming failed: VIDIOC_DQBUF") == 1);
		r.step([&] { r.sup.retryTimerFired(); });
		r.step([&] { r.sup.sourceEvent(false, {}); });
		REQUIRE(r.host.count(LogLevel::Warning, "source event notification without a readable event") == 1);
	});
	test("supervisor: explicit stop cancels everything; stale events are ignored", [] {
		for (auto entry : { FakeHost::Next::Ok, FakeHost::Next::NoSignal, FakeHost::Next::StreamFail })
		{
			Rig r; r.host.script = { entry };
			r.step([&] { r.sup.start(); });
			r.step([&] { r.sup.stop(); });
			REQUIRE(r.sup.state() == Supervisor::State::Stopped);
			REQUIRE(!r.host.capture && !r.host.polling && !r.host.waitOpen && r.host.retryMs < 0);
			const unsigned starts = r.host.starts, probes = r.host.probes;
			r.step([&] { r.sup.retryTimerFired(); r.sup.signalPollFired(); r.sup.stalled({ Signal::Absent, "x" });
				r.sup.sourceEvent(true, {}); r.sup.streamFailed("late worker"); });
			REQUIRE(r.host.starts == starts && r.host.probes == probes && r.sup.state() == Supervisor::State::Stopped);
			REQUIRE(!r.host.capture && !r.host.polling && r.host.retryMs < 0);
		}
		// A late retry or poll after a successful explicit start does nothing.
		Rig r; r.host.script = { FakeHost::Next::StreamFail };
		r.step([&] { r.sup.start(); });
		r.step([&] { r.sup.start(); });          // explicit start wins over the pending retry
		REQUIRE(r.sup.state() == Supervisor::State::Streaming && r.host.retryMs < 0);
		r.step([&] { r.sup.retryTimerFired(); r.sup.signalPollFired(); });
		REQUIRE(r.host.starts == 2);
		REQUIRE(r.sup.start() && r.host.starts == 2);   // already streaming
	});
	test("supervisor: vanished node during the wait falls back to discovery", [] {
		Rig r; r.host.script = { FakeHost::Next::NoSignal, FakeHost::Next::NoSignal };
		r.host.nodeOpens = false;
		r.step([&] { r.sup.start(); });
		REQUIRE(r.sup.state() == Supervisor::State::WaitingForSignal && r.host.polling && !r.host.waitOpen);
		r.step([&] { r.sup.signalPollFired(); });   // no handle: rediscover instead of probing
		REQUIRE(r.host.probes == 0 && r.host.starts == 2);
		REQUIRE(r.host.count(LogLevel::Info, "Cannot reopen /dev/video1") == 1);
		REQUIRE(r.sup.state() == Supervisor::State::WaitingForSignal);   // still dark
		r.host.nodeOpens = true; r.host.node.clear();
		r.step([&] { r.sup.stop(); r.host.script = { FakeHost::Next::NoSignal }; r.sup.start(); });
		REQUIRE(r.host.polling && !r.host.waitOpen);    // empty node: poll only
	});
	test("supervisor: explicit start forgets history; recovery start keeps it", [] {
		Rig r; r.host.script = { FakeHost::Next::StreamFail, FakeHost::Next::StreamFail, FakeHost::Next::StreamFail };
		r.step([&] { r.sup.start(); });
		r.step([&] { r.sup.retryTimerFired(); });
		REQUIRE(r.host.retryMs == 6000 && r.sup.recovery().failures() == 2);
		r.step([&] { r.sup.start(); });          // user re-enable: backoff restarts at 3 s
		REQUIRE(r.host.retryMs == 3000 && r.sup.recovery().failures() == 1);
		REQUIRE(r.host.count(LogLevel::Warning, "streaming failed") == 2);   // warned again after the reset
	});
	// ---- Recovery regression coverage (issues #11, #19) -----------------------------
	// Expected behaviour is taken from the supervisor's documented states; none of
	// these changes production code.
	test("supervisor: a retry that finds no signal waits instead of backing off", [] {
		using Next = FakeHost::Next;
		Rig r; r.host.script = { Next::StreamFail, Next::NoSignal };
		r.step([&] { r.sup.start(); });
		REQUIRE(r.sup.state() == Supervisor::State::Backoff && r.host.retryMs == 3000);
		r.step([&] { r.sup.retryTimerFired(); });
		// The receiver went dark while we were backing off: no timer keeps ticking.
		REQUIRE(r.sup.state() == Supervisor::State::WaitingForSignal);
		REQUIRE(r.host.retryMs < 0 && r.host.polling && r.host.waitOpen && !r.host.capture);
		const auto starts = r.host.starts;
		for (int i = 0; i < 20; ++i) r.step([&] { r.sup.signalPollFired(); });
		REQUIRE(r.host.starts == starts && r.host.probes == 20);       // probes only, no capture attempts
		r.host.waiting = { Signal::Present, "detected 3840x2160p" };
		r.step([&] { r.sup.signalPollFired(); });                      // signal returns
		REQUIRE(r.sup.state() == Supervisor::State::Streaming && r.host.capture && !r.host.polling && r.host.retryMs < 0);
		r.step([&] { r.sup.frameReceived(); });
		REQUIRE(r.host.count(LogLevel::Info, "Capture recovered after") == 1);
	});
	test("supervisor: events outside streaming change nothing", [] {
		using Next = FakeHost::Next;
		for (auto first : { Next::StreamFail, Next::NoSignal })
		{
			Rig r; r.host.script = { first };
			r.step([&] { r.sup.start(); });
			const auto state = r.sup.state();
			const int retry = r.host.retryMs;
			const unsigned starts = r.host.starts, stops = r.host.stops, probes = r.host.probes, failures = r.sup.recovery().failures();
			const size_t logs = r.host.logs.size();
			// A source change, stall, stream error or worker error that was queued before
			// teardown must not start, stop or re-arm anything.
			r.step([&] {
				r.sup.sourceEvent(true, { Signal::Present, "detected 3840x2160p" });
				r.sup.sourceEvent(false, {});
				r.sup.stalled({ Signal::Present, "detected 1920x1080p" });
				r.sup.stalled({ Signal::Absent, "no signal" });
				r.sup.streamFailed("late worker error");
			});
			REQUIRE(r.sup.state() == state && r.host.retryMs == retry);
			REQUIRE(r.host.starts == starts && r.host.stops == stops && r.host.probes == probes);
			REQUIRE(r.sup.recovery().failures() == failures && r.host.logs.size() == logs);
		}
	});
	test("supervisor: source change during recovery doesn't skip the backoff", [] {
		using Next = FakeHost::Next;
		Rig r; r.host.script = { Next::StreamFail, Next::StreamFail };
		r.step([&] { r.sup.start(); });
		r.step([&] { r.sup.retryTimerFired(); });
		REQUIRE(r.host.retryMs == 6000);
		r.step([&] { r.sup.sourceEvent(true, { Signal::Present, "detected 1920x1080p" }); });   // arrives while backing off
		REQUIRE(r.host.retryMs == 6000 && r.sup.state() == Supervisor::State::Backoff);
		r.step([&] { r.sup.retryTimerFired(); });                                               // script exhausted: succeeds
		REQUIRE(r.sup.state() == Supervisor::State::Streaming);
		// Once streaming again, a real source change takes the quick path.
		r.step([&] { r.sup.sourceEvent(true, { Signal::Present, "detected 3840x2160p" }); });
		REQUIRE(r.sup.state() == Supervisor::State::Backoff && r.host.retryMs == Recovery::sourceChangeDelayMs);
	});
	test("supervisor: device vanishing mid-stream and returning", [] {
		using Next = FakeHost::Next;
		Rig d;
		d.step([&] { REQUIRE(d.sup.start()); });
		REQUIRE(d.sup.state() == Supervisor::State::Streaming);
		d.host.script = { Next::InitFail, Next::InitFail, Next::InitFail };    // /dev node gone for three retries, then back
		d.step([&] { d.sup.streamFailed("streaming: VIDIOC_DQBUF: No such device"); });
		std::vector<int> delays{ d.host.retryMs };
		for (int i = 0; i < 3; ++i) { d.step([&] { d.sup.retryTimerFired(); }); delays.push_back(d.host.retryMs); }
		REQUIRE(delays == (std::vector<int>{ 3000, 6000, 12000, 24000 }));
		REQUIRE(d.sup.state() == Supervisor::State::Backoff);
		d.step([&] { d.sup.retryTimerFired(); });                              // fourth retry: node is back, script exhausted
		REQUIRE(d.sup.state() == Supervisor::State::Streaming);
		REQUIRE(d.host.capture && d.host.retryMs < 0 && !d.host.polling);
		d.step([&] { d.sup.frameReceived(); });
		REQUIRE(d.host.count(LogLevel::Info, "Capture recovered after") == 1);
	});
	test("supervisor: repeated start and stop cycles hold nothing", [] {
		using Next = FakeHost::Next;
		Rig r;
		const Next outcomes[] = { Next::Ok, Next::NoSignal, Next::StreamFail, Next::InitFail, Next::Generic };
		for (int i = 0; i < 400; ++i)
		{
			r.host.script = { outcomes[i % 5] };
			r.step([&] { r.sup.start(); });
			if (i % 7 == 0) r.step([&] { r.sup.retryTimerFired(); r.sup.signalPollFired(); });
			r.step([&] { r.sup.stop(); });
			REQUIRE(r.sup.state() == Supervisor::State::Stopped);
			REQUIRE(!r.host.capture && !r.host.polling && !r.host.waitOpen && r.host.retryMs < 0);   // no leaked handle, timer or wait
		}
		REQUIRE(r.host.starts >= 400);
		REQUIRE(r.host.stops >= r.host.starts - 1);   // every start was matched by a stop
	});
	test("supervisor: randomized events keep state and resources consistent", [] {
		using Next = FakeHost::Next;
		uint32_t seed = 0x2545f491u;
		auto rnd = [&] { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
		Rig r;
		unsigned lastStarts = 0, maxPending = 0;
		for (int step = 0; step < 40000; ++step)
		{
			const unsigned action = rnd() % 9;
			if (r.host.script.size() < 2 && rnd() % 2)
				r.host.script.push_back(static_cast<Next>(rnd() % 5));
			r.host.waiting = { rnd() % 3 ? Signal::Absent : Signal::Present, "random" };
			r.host.nodeOpens = rnd() % 4 != 0;
			const bool mayStart = action <= 2;
			switch (action)
			{
				case 0: r.sup.start(); break;
				case 1: r.sup.retryTimerFired(); break;
				case 2: r.sup.signalPollFired(); break;
				case 3: r.sup.stalled({ rnd() % 2 ? Signal::Absent : Signal::Present, "stall" }); break;
				case 4: r.sup.sourceEvent(rnd() % 2, { rnd() % 3 ? Signal::Present : Signal::Absent, "event" }); break;
				case 5: r.sup.streamFailed("random stream error"); break;
				case 6: r.sup.frameReceived(); break;
				case 7: if (rnd() % 4 == 0) r.sup.stop(); break;
				default: r.sup.shortFrame(); break;
			}
			r.host.invariant();                                             // at most one of capture / retry timer / signal wait
			maxPending = std::max(maxPending, unsigned(r.host.capture) + unsigned(r.host.retryMs >= 0) + unsigned(r.host.polling));
			switch (r.sup.state())
			{
				case Supervisor::State::Stopped:
					REQUIRE(!r.host.capture && !r.host.polling && !r.host.waitOpen && r.host.retryMs < 0); break;
				case Supervisor::State::Streaming:
					REQUIRE(r.host.capture && r.host.retryMs < 0 && !r.host.polling); break;
				case Supervisor::State::Backoff:
					REQUIRE(!r.host.capture && r.host.retryMs > 0 && !r.host.polling); break;
				case Supervisor::State::WaitingForSignal:
					REQUIRE(!r.host.capture && r.host.retryMs < 0 && r.host.polling); break;
			}
			// No busy polling or hidden restarts: a capture attempt happens only on an
			// explicit start, the retry timer or a signal poll.
			if (!mayStart) REQUIRE(r.host.starts == lastStarts);
			lastStarts = r.host.starts;
		}
		REQUIRE(maxPending <= 1);
	});
	test("1080p/4K source timing transitions: negotiation and payload sizes", [] {
		struct TimingDriver : Operations {
			v4l2_dv_timings detected{}, current{};
			unsigned sets = 0;
			TimingDriver() {
				detected.type = V4L2_DV_BT_656_1120; detected.bt.width = 1920; detected.bt.height = 1080;
				detected.bt.pixelclock = 148500000; current = detected;
			}
			int control(int, unsigned long request, void* arg) override {
				if (request == VIDIOC_QUERY_DV_TIMINGS) *static_cast<v4l2_dv_timings*>(arg) = detected;
				else if (request == VIDIOC_G_DV_TIMINGS) *static_cast<v4l2_dv_timings*>(arg) = current;
				else if (request == VIDIOC_S_DV_TIMINGS) { ++sets; current = *static_cast<v4l2_dv_timings*>(arg); }
				return 0;
			}
		} f;
		refreshTimings(f, 3); REQUIRE(f.sets == 0);                        // already 1080p: nothing to set
		f.detected.bt.width = 3840; f.detected.bt.height = 2160; f.detected.bt.pixelclock = 297000000;
		refreshTimings(f, 3);                                              // source moved to 4K30
		REQUIRE(f.sets == 1 && f.current.bt.width == 3840 && f.current.bt.height == 2160);
		refreshTimings(f, 3); REQUIRE(f.sets == 1);                        // settled: no repeated set
		f.detected.bt.width = 1920; f.detected.bt.height = 1080; f.detected.bt.pixelclock = 148500000;
		refreshTimings(f, 3); REQUIRE(f.sets == 2 && f.current.bt.width == 1920);   // and back

		// The negotiated frame sizes the grabber compares payloads against.
		auto negotiated = [](unsigned w, unsigned h) {
			v4l2_format fmt{}; fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			auto& p = fmt.fmt.pix_mp; p.width = w; p.height = h; p.pixelformat = V4L2_PIX_FMT_BGR24; p.num_planes = 1;
			p.plane_fmt[0].bytesperline = w * 3; p.plane_fmt[0].sizeimage = w * 3 * h;
			const auto native = format(fmt); validateFormat(native); return native;
		};
		const auto hd = negotiated(1920, 1080), uhd = negotiated(3840, 2160);
		REQUIRE(hd.size == 6220800 && hd.stride == 5760);
		REQUIRE(uhd.size == 24883200 && uhd.stride == 11520);              // matches the 4K30 capture measured on the board
		// A 1080p payload arriving after the receiver switched to 4K is short: dropped
		// when isolated, a restart when it persists.
		REQUIRE(hd.size < uhd.size);
		Recovery recovery;
		REQUIRE(!recovery.shortFrame() && !recovery.shortFrame() && recovery.shortFrame());
		REQUIRE(recovery.frameReceived() == 0 && !recovery.shortFrame());  // a good frame resets the run
	});
	std::cout << passed << " test groups passed\n";
}
