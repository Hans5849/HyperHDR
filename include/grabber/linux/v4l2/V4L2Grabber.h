#pragma once

#ifndef PCH_ENABLED
	#include <vector>
#endif

#include <grabber/linux/v4l2/V4L2Capture.h>
#include <grabber/linux/v4l2/V4L2Supervisor.h>
#include <grabber/linux/v4l2/V4L2Stats.h>
#include <memory>
#include <utils/PixelFormat.h>
#include <base/Grabber.h>
#include <grabber/GrabberWorker.h>
#include <utils/Components.h>

#include <turbojpeg.h>

class QSocketNotifier;
class QTimer;

class V4L2Grabber final : public Grabber
{
	Q_OBJECT

public:
	struct HyperHdrFormat
	{
		__u32		v4l2Format;
		PixelFormat innerFormat;
	};

	V4L2Grabber(const QString& device, const QString& configurationPath);

	~V4L2Grabber();

	int  grabFrame(Image<ColorRgb>&);

	void setHdrToneMappingEnabled(int mode) override;

public slots:

	bool start() override;

	void stop() override;

	void newWorkerFrameHandler(unsigned int workerIndex, Image<ColorRgb> image, quint64 sourceCount, qint64 _frameBegin) override;

	void newWorkerFrameErrorHandler(unsigned int workerIndex, QString error, quint64 sourceCount) override;

private slots:
	int read_frame();

private:
	QString GetSharedLut();

	void enumerateV4L2devices(bool silent);

	void loadLutFile(PixelFormat color = PixelFormat::NO_CHANGE, bool silent = false);

	void getV4L2devices();

	bool init() override;

	void uninit() override;

	void close_device();

	bool init_mmap();

	bool init_device(QString selectedDeviceName, DevicePropertiesItem props);

	void uninit_device();

	void start_capturing();

	// Capture lifecycle primitives, sequenced by V4L2Capture::Supervisor.
	struct SupervisorHost;
	void startCaptureOnce();
	void teardownCapture();
	bool openSignalWait(const QString& node);
	void closeSignalWait();
	V4L2Capture::SignalStatus probeWaitingSignal();
	V4L2Capture::SignalStatus captureSignal();
	void returnBuffer(unsigned int workerIndex, quint64 sourceCount);

	bool process_image(v4l2_buffer* buf, const void* frameImageBuffer, int size);

	int xioctl(int request, void* arg);

	int xioctl(int fileDescriptor, int request, void* arg);

	void throw_exception(const QString& error);

	void throw_errno_exception(const QString& error);

	QString formatRes(int w, int h, QString format) const;

	QString formatFrame(int fr);

	PixelFormat identifyFormat(__u32 format);

	bool getControl(int _fd, __u32 controlId, long& minVal, long& maxVal, long& defVal);

	bool setControl(__u32 controlId, __s32 newValue);

	QMap<QString, QString> _deviceAliases;
	size_t _captureImageSize = 0;
	int _fileDescriptor;
	V4L2Capture::Operations _operations;
	V4L2Capture::Buffers _buffers;
	v4l2_buf_type _captureType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	QTimer* _watchdog;
	QTimer* _retryTimer;
	QTimer* _signalTimer;
	QSocketNotifier* _signalNotifier = nullptr;
	int _signalFd = -1;
	QString _activeNode;
	QSocketNotifier* _eventNotifier = nullptr;
	qint64 _lastFrame = 0;
	quint64 _generationFrame = 0;
	quint64 _streamEpoch = 0;
	// Pipeline counters (docs/rk3588-pipeline-instrumentation.md). All updates are on
	// the grabber thread (worker signals are queued to it). The summary is logged
	// once a minute from read_frame and flushed whenever capture is torn down.
	V4L2Capture::Stats _stats;
	qint64 _statsIntervalBegin = 0;                // InternalClock::now() (ms) of the interval start; 0 = no interval
	V4L2Capture::AcceptTimes _acceptTimes;         // per worker: which frame was handed over, and when (us)
	void reportCaptureStats(qint64 now, const std::string& context = {});
	// Declared in this order so the supervisor is destroyed before its host.
	std::unique_ptr<SupervisorHost> _supervisorHost;
	std::unique_ptr<V4L2Capture::Supervisor> _supervisor;
	PixelFormat _loadedLutFormat = PixelFormat::NO_CHANGE;
	int _loadedLutHdr = -1;
	QSocketNotifier*	_streamNotifier;	
	GrabberManager      _V4L2WorkerManager;
};
