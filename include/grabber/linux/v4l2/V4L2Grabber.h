#pragma once

#ifndef PCH_ENABLED
	#include <vector>
#endif

#include <grabber/linux/v4l2/V4L2Capture.h>
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

	void restartCapture(V4L2Capture::Failure failure, const QString& detail, bool sourceChanged = false);
	void scheduleRetry(V4L2Capture::Failure failure, const QString& detail, bool sourceChanged = false);
	// No-signal handling for receivers that report their input state.
	bool pauseIfNoSignal(QString& signalDetail);
	void waitForSignal(const QString& node, const QString& detail);
	void checkSignal();
	void stopSignalWait();
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

	int _fileDescriptor;
	V4L2Capture::Operations _operations;
	V4L2Capture::Buffers _buffers;
	v4l2_buf_type _captureType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	QTimer* _watchdog;
	QTimer* _retryTimer;
	QTimer* _signalTimer;
	QSocketNotifier* _signalNotifier = nullptr;
	int _signalFd = -1;
	QString _signalNode;
	QString _activeNode;
	QSocketNotifier* _eventNotifier = nullptr;
	qint64 _lastFrame = 0;
	quint64 _generationFrame = 0;
	quint64 _streamEpoch = 0;
	V4L2Capture::Recovery _recovery;
	// Set while the retry timer restarts capture after a failure. Automatic
	// recovery reuses the LUT already in memory instead of reloading ~50 MB.
	bool _retrying = false;
	PixelFormat _loadedLutFormat = PixelFormat::NO_CHANGE;
	int _loadedLutHdr = -1;
	QSocketNotifier*	_streamNotifier;	
	GrabberManager      _V4L2WorkerManager;
};
