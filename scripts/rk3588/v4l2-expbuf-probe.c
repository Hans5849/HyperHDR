// Probe whether a V4L2 capture node can export its MMAP buffers as DMA-BUF
// (VIDIOC_EXPBUF). Used by collect-evidence.sh --expbuf for issue #13.
//
//   cc -O2 -o v4l2-expbuf-probe v4l2-expbuf-probe.c
//   ./v4l2-expbuf-probe /dev/videoN
//
// It requests two MMAP buffers, exports buffer 0 plane 0, closes the exported
// fd and frees the buffers again. It never streams. If another process owns
// the node (HyperHDR capturing), REQBUFS fails with EBUSY and nothing changes.
// Output is "key=value" lines; exit status 0 = probe ran, 2 = usage/open error.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long request, void* arg)
{
	int r;
	do r = ioctl(fd, request, arg); while (r < 0 && errno == EINTR);
	return r;
}

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		fprintf(stderr, "usage: %s /dev/videoN\n", argv[0]);
		return 2;
	}
	const int fd = open(argv[1], O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
	{
		printf("open=%s\n", strerror(errno));
		return 2;
	}

	struct v4l2_capability cap;
	memset(&cap, 0, sizeof(cap));
	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
	{
		printf("querycap=%s\n", strerror(errno));
		close(fd);
		return 2;
	}
	const unsigned caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
	enum v4l2_buf_type type;
	if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	else if (caps & V4L2_CAP_VIDEO_CAPTURE) type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	else
	{
		printf("capture=no\n");
		close(fd);
		return 0;
	}
	printf("driver=%.*s\nbus=%.*s\nbuffer_type=%s\n", (int)sizeof(cap.driver), (const char*)cap.driver,
		(int)sizeof(cap.bus_info), (const char*)cap.bus_info,
		type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? "VIDEO_CAPTURE_MPLANE" : "VIDEO_CAPTURE");

	struct v4l2_requestbuffers req;
	memset(&req, 0, sizeof(req));
	req.count = 2;
	req.type = type;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0)
	{
		printf("reqbufs=%s\n", strerror(errno));
		if (errno == EBUSY) printf("hint=node in use; stop HyperHDR to run this probe\n");
		close(fd);
		return 0;
	}
	printf("reqbufs=ok\nbuffers=%u\n", req.count);
#ifdef V4L2_BUF_CAP_SUPPORTS_DMABUF
	printf("supports_dmabuf_import=%s\n", (req.capabilities & V4L2_BUF_CAP_SUPPORTS_DMABUF) ? "yes" : "no");
#endif

	if (req.count > 0)
	{
		struct v4l2_exportbuffer exp;
		memset(&exp, 0, sizeof(exp));
		exp.type = type;
		exp.index = 0;
		exp.plane = 0;
		exp.flags = O_RDONLY | O_CLOEXEC;
		if (xioctl(fd, VIDIOC_EXPBUF, &exp) < 0)
			printf("expbuf=%s\n", strerror(errno));
		else
		{
			printf("expbuf=ok\n");
			close(exp.fd);
		}
	}

	memset(&req, 0, sizeof(req));
	req.count = 0;
	req.type = type;
	req.memory = V4L2_MEMORY_MMAP;
	printf("release=%s\n", xioctl(fd, VIDIOC_REQBUFS, &req) < 0 ? strerror(errno) : "ok");
	close(fd);
	return 0;
}
