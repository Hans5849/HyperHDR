#include <grabber/linux/v4l2/V4L2Capture.h>
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
	unsigned planeCount = 1, returnedCount = 4, index = 0, used = 24, offset = 0;
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
				for (unsigned p = 0; p < b.length; ++p) { b.m.planes[p].length = 32; b.m.planes[p].m.mem_offset = (b.index * planeCount + p) * 4096; }
			}
			else { b.length = 32; b.m.offset = b.index * 4096; }
		}
		if (request == VIDIOC_QBUF)
		{
			auto& b = *static_cast<v4l2_buffer*>(arg);
			if (b.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
			{
				REQUIRE(b.length == planeCount && b.m.planes != nullptr);
				for (unsigned p = 0; p < b.length; ++p) REQUIRE(b.m.planes[p].length == 32);
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
	std::cout << passed << " test groups passed\n";
}
