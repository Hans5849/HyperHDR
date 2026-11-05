#include <grabber/linux/v4l2/V4L2Capture.h>
#include <utils/Bgr24.h>
#include <functional>
#include <iostream>
#include <set>
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
	std::cout << passed << " test groups passed\n";
}
