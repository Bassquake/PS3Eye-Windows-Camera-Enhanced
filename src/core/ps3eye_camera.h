// PS3 Eye (OV534 bridge + OV7720 sensor) user-mode USB driver built on libusb.
//
// Sensor/bridge register tables, the frame-rate table and the payload parser come
// from the Linux gspca ov534 driver (GPL-2.0) by way of inspirit/PS3EYEDriver.
// The transport layer (transfers, threading, shutdown, hot-unplug handling) is
// rewritten here so that it never blocks forever and survives the camera being
// unplugged mid-stream.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

struct libusb_context;
struct libusb_device_handle;
struct libusb_transfer;

namespace ps3eye {

constexpr uint16_t kVendorId = 0x1415;
constexpr uint16_t kProductId = 0x2000;

// Gain, exposure, white balance and flips are set on the sensor. Brightness, contrast, saturation,
// hue and sharpness are done on the PC (convert.h, ImageAdjust): the sensor's own versions only
// work in its YUV mode, and the camera runs in raw Bayer mode.
struct Settings
{
	bool autoGain = true;          // auto gain + auto exposure
	bool autoWhiteBalance = true;
	int gain = 20;                 // 0..63   (used when autoGain is off)
	int exposure = 120;            // 0..255  (used when autoGain is off)
	int brightness = 128;          // 0..255, 128 = unchanged
	int contrast = 128;            // 0..255, 128 = unchanged
	int saturation = 128;          // 0..255, 128 = unchanged, 0 = black and white
	int hue = 0;                   // 0..359 degrees
	int sharpness = 0;             // 0..63, 0 = off
	int redBalance = 128;          // 0..255, used when autoWhiteBalance is off
	int greenBalance = 128;
	int blueBalance = 128;
	bool flipH = false;
	bool flipV = false;
};

enum class Status
{
	Ok,
	NotFound,       // no PS3 Eye plugged in
	NoDriver,       // plugged in but WinUSB/libusb driver not installed on interface 0
	AccessDenied,
	Busy,           // another program has the camera open
	InitFailed,
	Error,
};

const char* StatusText(Status s);

class Camera
{
public:
	Camera() = default;
	~Camera();
	Camera(const Camera&) = delete;
	Camera& operator=(const Camera&) = delete;

	// Number of PS3 Eye cameras on the bus (driver installed or not).
	static int Count();

	// Checks whether camera #index can be opened (driver installed, not in use) without streaming.
	static Status Probe(int index = 0);

	// Opens camera #index and starts streaming. width/height: 640x480 or 320x240.
	// fps is rounded down to the nearest rate the sensor supports.
	Status Start(uint32_t width, uint32_t height, uint32_t fps, const Settings& settings, int index = 0);
	void Stop();

	bool IsStreaming() const { return _streaming; }
	bool DeviceLost() const { return _lost; }

	// Waits for a frame newer than the last one returned. The frame is raw Bayer
	// (GRBG), width*height bytes. Returns false on timeout or when the device is lost.
	bool WaitFrame(std::vector<uint8_t>& bayer, std::chrono::milliseconds timeout);

	// Applies settings while streaming.
	void Apply(const Settings& settings);

	uint32_t Width() const { return _width; }
	uint32_t Height() const { return _height; }
	uint32_t Fps() const { return _fps; }

	// Picks the real sensor rate for a requested rate (same rule Start uses). All rates: rates.h.
	static uint32_t SupportedFps(uint32_t width, uint32_t fps);

	// internal: called from the libusb event thread
	void OnTransfer(libusb_transfer* xfr);

#ifdef PS3EYE_TESTING
	void TestBegin(uint32_t w, uint32_t h)
	{
		_width = w; _height = h; _frameSize = w * h;
		_assembling.assign(_frameSize, 0); _latest.assign(_frameSize, 0);
		_assembledLen = 0; _lastPacketType = 0; _lastPts = 0; _lastFid = 0; _seq = _seqDelivered = 0;
		_streaming = true;
	}
	void TestFeed(const uint8_t* data, int len) { ScanPayload(data, len); }
	void TestEnd() { _streaming = false; }
#endif

private:
	void EventLoop();
	void ScanPayload(const uint8_t* data, int len);
	void FrameAdd(int packetType, const uint8_t* data, int len);
	void Cleanup();

	// register access
	bool BridgeWrite(uint16_t reg, uint8_t val);
	uint8_t BridgeRead(uint16_t reg);
	bool SccbWait();
	void SccbWrite(uint8_t reg, uint8_t val);
	uint8_t SccbRead(uint8_t reg);
	void BridgeWriteArray(const uint8_t (*data)[2], size_t len);
	void SccbWriteArray(const uint8_t (*data)[2], size_t len);
	void SetLed(bool on);
	uint32_t SetFrameRate(uint32_t fps, bool dryRun);
	void ApplyLocked(const Settings& s);

	libusb_context* _ctx = nullptr;
	libusb_device_handle* _handle = nullptr;
	bool _claimed = false;
	uint8_t _endpoint = 0;

	uint32_t _width = 0, _height = 0, _fps = 0, _frameSize = 0;
	std::atomic<bool> _streaming{ false };
	std::atomic<bool> _stopping{ false };
	std::atomic<bool> _lost{ false };
	std::atomic<bool> _quitEvents{ false };
	std::thread _eventThread;

	static constexpr int kNumTransfers = 5;
	static constexpr int kTransferSize = 65536;
	libusb_transfer* _xfr[kNumTransfers] = {};
	std::vector<uint8_t> _xfrBuffer;
	std::mutex _activeMutex;
	std::condition_variable _activeCv;
	int _active = 0;

	// frame assembly (event thread only)
	std::vector<uint8_t> _assembling;
	uint32_t _assembledLen = 0;
	int _lastPacketType = 0;
	uint32_t _lastPts = 0;
	uint16_t _lastFid = 0;

	// latest complete frame (shared with consumer)
	std::mutex _frameMutex;
	std::condition_variable _frameCv;
	std::vector<uint8_t> _latest;
	uint64_t _seq = 0, _seqDelivered = 0;

	std::mutex _regMutex;   // serializes register access
	Settings _settings;
};

} // namespace ps3eye
