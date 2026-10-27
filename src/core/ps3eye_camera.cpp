#include "ps3eye_camera.h"

#include <algorithm>
#include <cstring>
#include <iterator>

#ifdef _WIN32
#include <winsock2.h> // struct timeval used by libusb_handle_events_timeout_completed
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4200) // zero-sized array in libusb.h
#endif
#include "libusb.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace ps3eye {

namespace {

constexpr uint16_t OV534_REG_ADDRESS = 0xf1;
constexpr uint16_t OV534_REG_SUBADDR = 0xf2;
constexpr uint16_t OV534_REG_WRITE = 0xf3;
constexpr uint16_t OV534_REG_READ = 0xf4;
constexpr uint16_t OV534_REG_OPERATION = 0xf5;
constexpr uint16_t OV534_REG_STATUS = 0xf6;
constexpr uint8_t OV534_OP_WRITE_3 = 0x37;
constexpr uint8_t OV534_OP_WRITE_2 = 0x33;
constexpr uint8_t OV534_OP_READ_2 = 0xf9;

constexpr uint8_t kVendorOut = uint8_t(LIBUSB_ENDPOINT_OUT) | uint8_t(LIBUSB_REQUEST_TYPE_VENDOR) | uint8_t(LIBUSB_RECIPIENT_DEVICE);
constexpr uint8_t kVendorIn = uint8_t(LIBUSB_ENDPOINT_IN) | uint8_t(LIBUSB_REQUEST_TYPE_VENDOR) | uint8_t(LIBUSB_RECIPIENT_DEVICE);

template <size_t N> constexpr size_t countof(const uint8_t (&)[N][2]) { return N; }

const uint8_t ov534_reg_initdata[][2] = {
	{ 0xe7, 0x3a },
	{ OV534_REG_ADDRESS, 0x42 }, // select OV772x sensor
	{ 0x92, 0x01 }, { 0x93, 0x18 }, { 0x94, 0x10 }, { 0x95, 0x10 }, { 0xE2, 0x00 }, { 0xE7, 0x3E },
	{ 0x96, 0x00 }, { 0x97, 0x20 }, { 0x97, 0x20 }, { 0x97, 0x20 }, { 0x97, 0x0A }, { 0x97, 0x3F },
	{ 0x97, 0x4A }, { 0x97, 0x20 }, { 0x97, 0x15 }, { 0x97, 0x0B },
	{ 0x8E, 0x40 }, { 0x1F, 0x81 }, { 0xC0, 0x50 }, { 0xC1, 0x3C }, { 0xC2, 0x01 }, { 0xC3, 0x01 },
	{ 0x50, 0x89 }, { 0x88, 0x08 }, { 0x8D, 0x00 }, { 0x8E, 0x00 },
	{ 0x1C, 0x00 },                                 // video data start (V_FMT)
	{ 0x1D, 0x00 },                                 // RAW8 mode
	{ 0x1D, 0x02 }, { 0x1D, 0x00 },                 // payload size 0x0200 * 4 = 2048 bytes
	{ 0x1D, 0x01 }, { 0x1D, 0x2C }, { 0x1D, 0x00 }, // frame size 0x012C00 * 4 = 307200 bytes
	{ 0x1C, 0x0A },                                 // video data start (V_CNTL0)
	{ 0x1D, 0x08 },                                 // turn on UVC header
	{ 0x1D, 0x0E },
	{ 0x34, 0x05 }, { 0xE3, 0x04 }, { 0x89, 0x00 }, { 0x76, 0x00 }, { 0xE7, 0x2E }, { 0x31, 0xF9 },
	{ 0x25, 0x42 }, { 0x21, 0xF0 }, { 0xE5, 0x04 },
};

const uint8_t ov772x_reg_initdata[][2] = {
	{ 0x12, 0x80 }, // reset
	{ 0x3D, 0x00 },
	{ 0x12, 0x01 }, // processed Bayer RAW (8bit)
	{ 0x11, 0x01 }, { 0x14, 0x40 }, { 0x15, 0x00 }, { 0x63, 0xAA }, { 0x64, 0x87 }, { 0x66, 0x00 },
	{ 0x67, 0x02 }, { 0x17, 0x26 }, { 0x18, 0xA0 }, { 0x19, 0x07 }, { 0x1A, 0xF0 }, { 0x29, 0xA0 },
	{ 0x2A, 0x00 }, { 0x2C, 0xF0 }, { 0x20, 0x10 }, { 0x4E, 0x0F }, { 0x3E, 0xF3 }, { 0x0D, 0x41 },
	{ 0x32, 0x00 }, { 0x13, 0xF0 }, { 0x22, 0x7F }, { 0x23, 0x03 }, { 0x24, 0x40 }, { 0x25, 0x30 },
	{ 0x26, 0xA1 }, { 0x2A, 0x00 }, { 0x2B, 0x00 }, { 0x13, 0xF7 }, { 0x0C, 0xC0 },
	{ 0x11, 0x00 }, { 0x0D, 0x41 },
	{ 0x8E, 0x00 }, // de-noise threshold
};

const uint8_t bridge_start_vga[][2] = {
	{ 0x1c, 0x00 }, { 0x1d, 0x00 }, { 0x1d, 0x02 }, { 0x1d, 0x00 },
	{ 0x1d, 0x01 }, { 0x1d, 0x2C }, { 0x1d, 0x00 }, // 640*480
	{ 0xc0, 0x50 }, { 0xc1, 0x3c },
};
const uint8_t sensor_start_vga[][2] = {
	{ 0x12, 0x01 }, { 0x17, 0x26 }, { 0x18, 0xa0 }, { 0x19, 0x07 },
	{ 0x1a, 0xf0 }, { 0x29, 0xa0 }, { 0x2c, 0xf0 }, { 0x65, 0x20 },
};
const uint8_t bridge_start_qvga[][2] = {
	{ 0x1c, 0x00 }, { 0x1d, 0x00 }, { 0x1d, 0x02 }, { 0x1d, 0x00 },
	{ 0x1d, 0x00 }, { 0x1d, 0x4b }, { 0x1d, 0x00 }, // 320*240
	{ 0xc0, 0x28 }, { 0xc1, 0x1e },
};
const uint8_t sensor_start_qvga[][2] = {
	{ 0x12, 0x41 }, { 0x17, 0x3f }, { 0x18, 0x50 }, { 0x19, 0x03 },
	{ 0x1a, 0x78 }, { 0x29, 0x50 }, { 0x2c, 0x78 }, { 0x65, 0x2f },
};

struct Rate { uint16_t fps; uint8_t r11, r0d, re5; };

// Entries that produce corrupt video (83 fps VGA, 205/290 fps QVGA) are left out.
const Rate rate_vga[] = {
	{ 75, 0x01, 0x81, 0x02 }, { 60, 0x00, 0x41, 0x04 }, { 50, 0x01, 0x41, 0x02 },
	{ 40, 0x02, 0xc1, 0x04 }, { 30, 0x04, 0x81, 0x02 }, { 25, 0x00, 0x01, 0x02 },
	{ 20, 0x04, 0x41, 0x02 }, { 15, 0x09, 0x81, 0x02 }, { 10, 0x09, 0x41, 0x02 },
	{ 8, 0x02, 0x01, 0x02 },  { 5, 0x04, 0x01, 0x02 },  { 3, 0x06, 0x01, 0x02 },
	{ 2, 0x09, 0x01, 0x02 },
};
const Rate rate_qvga[] = {
	{ 187, 0x01, 0x81, 0x02 }, { 150, 0x00, 0x41, 0x04 }, { 137, 0x02, 0xc1, 0x02 },
	{ 125, 0x01, 0x41, 0x02 }, { 100, 0x02, 0xc1, 0x04 }, { 90, 0x03, 0x81, 0x02 },
	{ 75, 0x04, 0x81, 0x02 },  { 60, 0x04, 0xc1, 0x04 },  { 50, 0x04, 0x41, 0x02 },
	{ 40, 0x06, 0x81, 0x03 },  { 37, 0x00, 0x01, 0x04 },  { 30, 0x04, 0x41, 0x04 },
	{ 17, 0x18, 0xc1, 0x02 },  { 15, 0x18, 0x81, 0x02 },  { 12, 0x02, 0x01, 0x04 },
	{ 10, 0x18, 0x41, 0x02 },  { 7, 0x04, 0x01, 0x04 },   { 5, 0x06, 0x01, 0x04 },
	{ 3, 0x09, 0x01, 0x04 },   { 2, 0x18, 0x01, 0x02 },
};

const Rate& PickRate(uint32_t width, uint32_t fps)
{
	const Rate* table = width == 640 ? rate_vga : rate_qvga;
	size_t n = width == 640 ? std::size(rate_vga) : std::size(rate_qvga);
	for (size_t i = 0; i < n; i++)
	{
		if (fps >= table[i].fps)
			return table[i];
	}
	return table[n - 1];
}

// UVC payload header flags
constexpr uint8_t UVC_STREAM_ERR = 1 << 6;
constexpr uint8_t UVC_STREAM_PTS = 1 << 2;
constexpr uint8_t UVC_STREAM_EOF = 1 << 1;
constexpr uint8_t UVC_STREAM_FID = 1 << 0;

enum PacketType { DISCARD_PACKET, FIRST_PACKET, INTER_PACKET, LAST_PACKET };

uint8_t FindBulkEndpoint(libusb_device* device)
{
	libusb_config_descriptor* config = nullptr;
	if (libusb_get_active_config_descriptor(device, &config) != 0 || !config)
		return 0;

	uint8_t addr = 0;
	for (int i = 0; i < config->bNumInterfaces && !addr; i++)
	{
		const libusb_interface_descriptor* alt = config->interface[i].altsetting;
		if (!alt || alt->bInterfaceNumber != 0)
			continue;
		for (int e = 0; e < alt->bNumEndpoints; e++)
		{
			const libusb_endpoint_descriptor& ep = alt->endpoint[e];
			if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK && ep.wMaxPacketSize)
			{
				addr = ep.bEndpointAddress;
				break;
			}
		}
	}
	libusb_free_config_descriptor(config);
	return addr;
}

Status MapError(int err)
{
	switch (err)
	{
	case LIBUSB_ERROR_ACCESS: return Status::AccessDenied;
	case LIBUSB_ERROR_BUSY: return Status::Busy;
	case LIBUSB_ERROR_NOT_SUPPORTED: return Status::NoDriver; // Windows: no WinUSB driver bound
	case LIBUSB_ERROR_NOT_FOUND: return Status::NoDriver;
	case LIBUSB_ERROR_NO_DEVICE: return Status::NotFound;
	default: return Status::Error;
	}
}

void LIBUSB_CALL TransferCallback(libusb_transfer* xfr)
{
	static_cast<Camera*>(xfr->user_data)->OnTransfer(xfr);
}

} // namespace

const char* StatusText(Status s)
{
	switch (s)
	{
	case Status::Ok: return "OK";
	case Status::NotFound: return "No PS3 Eye camera found. Check the USB cable.";
	case Status::NoDriver: return "PS3 Eye found, but its USB driver isn't installed. Run PS3EyeSetup and choose Install.";
	case Status::AccessDenied: return "Access denied: the camera is in use by another program, or the driver is wrong.";
	case Status::Busy: return "The camera is in use by another program.";
	case Status::InitFailed: return "The camera did not respond during initialization. Unplug it, plug it back in and retry.";
	default: return "USB error.";
	}
}

uint32_t Camera::SupportedFps(uint32_t width, uint32_t fps)
{
	return PickRate(width <= 320 ? 320 : 640, fps).fps;
}

int Camera::Count()
{
	libusb_context* ctx = nullptr;
	if (libusb_init(&ctx) != 0)
		return 0;

	libusb_device** list = nullptr;
	ssize_t n = libusb_get_device_list(ctx, &list);
	int count = 0;
	for (ssize_t i = 0; i < n; i++)
	{
		libusb_device_descriptor desc{};
		if (libusb_get_device_descriptor(list[i], &desc) == 0 && desc.idVendor == kVendorId && desc.idProduct == kProductId)
			count++;
	}
	if (list)
		libusb_free_device_list(list, 1);
	libusb_exit(ctx);
	return count;
}

Status Camera::Probe(int index)
{
	libusb_context* ctx = nullptr;
	if (libusb_init(&ctx) != 0)
		return Status::Error;

	libusb_device** list = nullptr;
	ssize_t n = libusb_get_device_list(ctx, &list);
	Status result = Status::NotFound;
	int found = 0;
	for (ssize_t i = 0; i < n; i++)
	{
		libusb_device_descriptor desc{};
		if (libusb_get_device_descriptor(list[i], &desc) != 0 || desc.idVendor != kVendorId || desc.idProduct != kProductId)
			continue;
		if (found++ != index)
			continue;

		libusb_device_handle* h = nullptr;
		int err = libusb_open(list[i], &h);
		if (err == 0)
		{
			// on Windows, open can succeed on a composite device whose video interface has no driver;
			// claiming interface 0 is the real test
			err = libusb_claim_interface(h, 0);
			if (err == 0)
				libusb_release_interface(h, 0);
			result = err == 0 ? Status::Ok : MapError(err);
			libusb_close(h);
		}
		else
		{
			result = MapError(err);
		}
		break;
	}
	if (list)
		libusb_free_device_list(list, 1);
	libusb_exit(ctx);
	return result;
}

Camera::~Camera()
{
	Stop();
}

Status Camera::Start(uint32_t width, uint32_t height, uint32_t fps, const Settings& settings, int index)
{
	Stop();
	_lost = false;
	_stopping = false;
	_quitEvents = false;

	if (libusb_init(&_ctx) != 0)
	{
		_ctx = nullptr;
		return Status::Error;
	}

	// find the camera
	libusb_device** list = nullptr;
	ssize_t n = libusb_get_device_list(_ctx, &list);
	libusb_device* dev = nullptr;
	int found = 0;
	for (ssize_t i = 0; i < n && !dev; i++)
	{
		libusb_device_descriptor desc{};
		if (libusb_get_device_descriptor(list[i], &desc) == 0 && desc.idVendor == kVendorId && desc.idProduct == kProductId)
		{
			if (found++ == index)
				dev = list[i];
		}
	}

	if (!dev)
	{
		if (list)
			libusb_free_device_list(list, 1);
		Cleanup();
		return Status::NotFound;
	}

	int err = libusb_open(dev, &_handle);
	if (err == 0)
		_endpoint = FindBulkEndpoint(dev);
	libusb_free_device_list(list, 1);
	if (err != 0)
	{
		_handle = nullptr;
		Cleanup();
		return MapError(err);
	}

#if !defined(_WIN32)
	libusb_set_auto_detach_kernel_driver(_handle, 1); // Linux: detach gspca_ov534
#endif

	err = libusb_claim_interface(_handle, 0);
	if (err != 0)
	{
		Cleanup();
		return MapError(err);
	}
	_claimed = true;

	if (!_endpoint)
	{
		Cleanup();
		return Status::InitFailed;
	}

	if (width <= 320 && height <= 240)
	{
		_width = 320;
		_height = 240;
	}
	else
	{
		_width = 640;
		_height = 480;
	}
	_frameSize = _width * _height;
	_fps = SupportedFps(_width, fps);
	_settings = settings;

	bool ok = true;
	{
		std::lock_guard<std::mutex> lock(_regMutex);

		// reset bridge
		ok &= BridgeWrite(0xe7, 0x3a);
		ok &= BridgeWrite(0xe0, 0x08);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

		// sensor address + reset
		ok &= BridgeWrite(OV534_REG_ADDRESS, 0x42);
		SccbWrite(0x12, 0x80);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));

		// probe the sensor (first read of each register primes the bus)
		SccbRead(0x0a);
		SccbRead(0x0a);
		SccbRead(0x0b);
		SccbRead(0x0b);

		BridgeWriteArray(ov534_reg_initdata, countof(ov534_reg_initdata));
		SetLed(true);
		SccbWriteArray(ov772x_reg_initdata, countof(ov772x_reg_initdata));
		ok &= BridgeWrite(0xe0, 0x09);
		SetLed(false);

		// start streaming
		if (_width == 320)
		{
			BridgeWriteArray(bridge_start_qvga, countof(bridge_start_qvga));
			SccbWriteArray(sensor_start_qvga, countof(sensor_start_qvga));
		}
		else
		{
			BridgeWriteArray(bridge_start_vga, countof(bridge_start_vga));
			SccbWriteArray(sensor_start_vga, countof(sensor_start_vga));
		}
		SetFrameRate(_fps, false);
		ApplyLocked(_settings);
		SetLed(true);
		ok &= BridgeWrite(0xe0, 0x00); // start stream
	}

	if (!ok || _lost)
	{
		Cleanup();
		return _lost ? Status::NotFound : Status::InitFailed;
	}

	// frame buffers
	_assembling.assign(_frameSize, 0);
	_latest.assign(_frameSize, 0);
	_assembledLen = 0;
	_lastPacketType = DISCARD_PACKET;
	_lastPts = 0;
	_lastFid = 0;
	{
		std::lock_guard<std::mutex> lock(_frameMutex);
		_seq = _seqDelivered = 0;
	}

	// bulk transfers
	libusb_clear_halt(_handle, _endpoint);
	_xfrBuffer.assign((size_t)kTransferSize * kNumTransfers, 0);
	_active = 0;
	for (int i = 0; i < kNumTransfers; i++)
	{
		_xfr[i] = libusb_alloc_transfer(0);
		if (!_xfr[i])
			continue;
		libusb_fill_bulk_transfer(_xfr[i], _handle, _endpoint, _xfrBuffer.data() + (size_t)i * kTransferSize, kTransferSize, TransferCallback, this, 0);
		std::lock_guard<std::mutex> lock(_activeMutex);
		if (libusb_submit_transfer(_xfr[i]) == 0)
			_active++;
	}

	if (_active == 0)
	{
		for (auto& x : _xfr)
		{
			if (x)
				libusb_free_transfer(x);
			x = nullptr;
		}
		Cleanup();
		return Status::InitFailed;
	}

	_eventThread = std::thread(&Camera::EventLoop, this);
	_streaming = true;
	return Status::Ok;
}

void Camera::Stop()
{
	if (_streaming)
	{
		_stopping = true;

		if (!_lost)
		{
			std::lock_guard<std::mutex> lock(_regMutex);
			BridgeWrite(0xe0, 0x09); // stop stream
			SetLed(false);
		}

		// cancel transfers; the event thread is still running and delivers the cancellations
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		std::unique_lock<std::mutex> lock(_activeMutex);
		while (_active > 0 && std::chrono::steady_clock::now() < deadline)
		{
			lock.unlock();
			for (auto x : _xfr)
			{
				if (x)
					libusb_cancel_transfer(x);
			}
			lock.lock();
			_activeCv.wait_for(lock, std::chrono::milliseconds(100), [this] { return _active == 0; });
		}
		bool leaked = _active > 0;
		lock.unlock();

		_quitEvents = true;
		if (_eventThread.joinable())
			_eventThread.join();

		for (auto& x : _xfr)
		{
			// if a transfer is somehow still pending, leak it rather than free memory libusb still owns
			if (x && !leaked)
				libusb_free_transfer(x);
			x = nullptr;
		}
		if (leaked)
			new std::vector<uint8_t>(std::move(_xfrBuffer)); // intentionally leaked with the transfers
		else
			_xfrBuffer.clear();

		_streaming = false;
	}

	Cleanup();
	_frameCv.notify_all();
}

void Camera::Cleanup()
{
	if (_handle)
	{
		if (_claimed)
			libusb_release_interface(_handle, 0);
		libusb_close(_handle);
	}
	_claimed = false;
	_handle = nullptr;
	if (_ctx)
		libusb_exit(_ctx);
	_ctx = nullptr;
}

void Camera::EventLoop()
{
	timeval tv{ 0, 50 * 1000 };
	while (!_quitEvents)
	{
		int r = libusb_handle_events_timeout_completed(_ctx, &tv, nullptr);
		if (r != 0 && r != LIBUSB_ERROR_INTERRUPTED)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

void Camera::OnTransfer(libusb_transfer* xfr)
{
	bool resubmit = false;
	switch (xfr->status)
	{
	case LIBUSB_TRANSFER_COMPLETED:
		if (!_stopping)
		{
			ScanPayload(xfr->buffer, xfr->actual_length);
			resubmit = true;
		}
		break;

	case LIBUSB_TRANSFER_CANCELLED:
		break;

	case LIBUSB_TRANSFER_OVERFLOW:
	case LIBUSB_TRANSFER_TIMED_OUT:
		resubmit = !_stopping;
		break;

	default: // NO_DEVICE (unplugged), ERROR, STALL
		if (!_stopping)
			_lost = true;
		break;
	}

	if (resubmit && !_lost)
	{
		if (libusb_submit_transfer(xfr) == 0)
			return;
		_lost = true;
	}

	{
		std::lock_guard<std::mutex> lock(_activeMutex);
		_active--;
	}
	_activeCv.notify_all();

	if (_lost)
	{
		std::lock_guard<std::mutex> lock(_frameMutex);
		_frameCv.notify_all();
	}
}

bool Camera::WaitFrame(std::vector<uint8_t>& bayer, std::chrono::milliseconds timeout)
{
	std::unique_lock<std::mutex> lock(_frameMutex);
	_frameCv.wait_for(lock, timeout, [this] { return _seq != _seqDelivered || _lost || _stopping || !_streaming; });
	if (_seq == _seqDelivered)
		return false;

	bayer.resize(_frameSize);
	std::swap(bayer, _latest); // zero-copy hand-off; _latest keeps a frame-sized buffer
	_seqDelivered = _seq;
	return true;
}

// Payloads are 2048 bytes, each prefixed with a 12-byte UVC-style header. A frame starts
// when FID toggles or PTS changes and ends at EOF once exactly width*height bytes arrived.
void Camera::ScanPayload(const uint8_t* data, int remaining)
{
	const int payloadLen = 2048;
	while (remaining > 0)
	{
		int len = (std::min)(remaining, payloadLen);
		bool discard = false;

		if (len < 12 || data[0] != 12 || (data[1] & UVC_STREAM_ERR) || !(data[1] & UVC_STREAM_PTS))
		{
			discard = true;
		}
		else
		{
			uint32_t pts = (uint32_t)data[5] << 24 | (uint32_t)data[4] << 16 | (uint32_t)data[3] << 8 | data[2];
			uint16_t fid = (data[1] & UVC_STREAM_FID) ? 1 : 0;

			if (pts != _lastPts || fid != _lastFid)
			{
				if (_lastPacketType == INTER_PACKET)
					FrameAdd(DISCARD_PACKET, nullptr, 0); // previous frame incomplete
				_lastPts = pts;
				_lastFid = fid;
				FrameAdd(FIRST_PACKET, data + 12, len - 12);
			}
			else if (data[1] & UVC_STREAM_EOF)
			{
				_lastPts = 0;
				if (_assembledLen + (uint32_t)(len - 12) != _frameSize)
					discard = true;
				else
					FrameAdd(LAST_PACKET, data + 12, len - 12);
			}
			else
			{
				FrameAdd(INTER_PACKET, data + 12, len - 12);
			}
		}

		if (discard)
			FrameAdd(DISCARD_PACKET, nullptr, 0);

		remaining -= len;
		data += len;
	}
}

void Camera::FrameAdd(int type, const uint8_t* data, int len)
{
	if (type == FIRST_PACKET)
	{
		_assembledLen = 0;
	}
	else
	{
		if (_lastPacketType == DISCARD_PACKET)
		{
			if (type == LAST_PACKET)
			{
				_lastPacketType = type;
				_assembledLen = 0;
			}
			return;
		}
		if (_lastPacketType == LAST_PACKET)
			return;
	}

	if (len > 0)
	{
		if (_assembledLen + (uint32_t)len > _frameSize)
		{
			type = DISCARD_PACKET;
			_assembledLen = 0;
		}
		else
		{
			memcpy(_assembling.data() + _assembledLen, data, len);
			_assembledLen += len;
		}
	}

	_lastPacketType = type;

	if (type == LAST_PACKET)
	{
		_assembledLen = 0;
		{
			std::lock_guard<std::mutex> lock(_frameMutex);
			std::swap(_assembling, _latest);
			_seq++;
		}
		_frameCv.notify_all();
	}
}

// ---- register access -------------------------------------------------------

bool Camera::BridgeWrite(uint16_t reg, uint8_t val)
{
	if (!_handle || _lost)
		return false;
	uint8_t buf = val;
	int r = libusb_control_transfer(_handle, kVendorOut, 0x01, 0x00, reg, &buf, 1, 500);
	if (r == LIBUSB_ERROR_NO_DEVICE)
		_lost = true;
	return r >= 0;
}

uint8_t Camera::BridgeRead(uint16_t reg)
{
	if (!_handle || _lost)
		return 0;
	uint8_t buf = 0;
	int r = libusb_control_transfer(_handle, kVendorIn, 0x01, 0x00, reg, &buf, 1, 500);
	if (r == LIBUSB_ERROR_NO_DEVICE)
		_lost = true;
	return r >= 0 ? buf : 0;
}

bool Camera::SccbWait()
{
	for (int i = 0; i < 5 && !_lost; i++)
	{
		uint8_t s = BridgeRead(OV534_REG_STATUS);
		if (s == 0x00)
			return true;
		if (s == 0x04)
			return false;
	}
	return false;
}

void Camera::SccbWrite(uint8_t reg, uint8_t val)
{
	BridgeWrite(OV534_REG_SUBADDR, reg);
	BridgeWrite(OV534_REG_WRITE, val);
	BridgeWrite(OV534_REG_OPERATION, OV534_OP_WRITE_3);
	SccbWait();
}

uint8_t Camera::SccbRead(uint8_t reg)
{
	BridgeWrite(OV534_REG_SUBADDR, reg);
	BridgeWrite(OV534_REG_OPERATION, OV534_OP_WRITE_2);
	SccbWait();
	BridgeWrite(OV534_REG_OPERATION, OV534_OP_READ_2);
	SccbWait();
	return BridgeRead(OV534_REG_READ);
}

void Camera::BridgeWriteArray(const uint8_t (*data)[2], size_t len)
{
	for (size_t i = 0; i < len; i++)
		BridgeWrite(data[i][0], data[i][1]);
}

void Camera::SccbWriteArray(const uint8_t (*data)[2], size_t len)
{
	for (size_t i = 0; i < len; i++)
	{
		if (data[i][0] != 0xff)
		{
			SccbWrite(data[i][0], data[i][1]);
		}
		else
		{
			SccbRead(data[i][1]);
			SccbWrite(0xff, 0x00);
		}
	}
}

// LED is driven by bit 7 of bridge registers 0x21 (direction) and 0x23 (level)
void Camera::SetLed(bool on)
{
	BridgeWrite(0x21, BridgeRead(0x21) | 0x80);
	uint8_t v = BridgeRead(0x23);
	BridgeWrite(0x23, on ? (v | 0x80) : (v & ~0x80));
	if (!on)
		BridgeWrite(0x21, BridgeRead(0x21) & ~0x80);
}

uint32_t Camera::SetFrameRate(uint32_t fps, bool dryRun)
{
	const Rate& r = PickRate(_width, fps);
	if (!dryRun)
	{
		SccbWrite(0x11, r.r11);
		SccbWrite(0x0d, r.r0d);
		BridgeWrite(0xe5, r.re5);
	}
	return r.fps;
}

void Camera::Apply(const Settings& s)
{
	if (!_streaming || _lost)
		return;
	std::lock_guard<std::mutex> lock(_regMutex);
	ApplyLocked(s);
}

void Camera::ApplyLocked(const Settings& s)
{
	_settings = s;

	if (s.autoGain)
	{
		SccbWrite(0x13, 0xf7); // AGC, AEC, AWB on
		SccbWrite(0x64, SccbRead(0x64) | 0x03);
	}
	else
	{
		SccbWrite(0x13, 0xf0); // AGC, AEC, AWB off
		SccbWrite(0x64, SccbRead(0x64) & 0xFC);

		uint8_t g = (uint8_t)std::clamp(s.gain, 0, 63);
		switch (g & 0x30)
		{
		case 0x00: g &= 0x0F; break;
		case 0x10: g = (g & 0x0F) | 0x30; break;
		case 0x20: g = (g & 0x0F) | 0x70; break;
		case 0x30: g = (g & 0x0F) | 0xF0; break;
		}
		const int exposure = std::clamp(s.exposure, 0, 255);
		SccbWrite(0x00, g);
		SccbWrite(0x08, (uint8_t)(exposure >> 7));
		SccbWrite(0x10, (uint8_t)(exposure << 1));
	}

	SccbWrite(0x63, s.autoWhiteBalance ? 0xe0 : 0xAA);
	if (!s.autoWhiteBalance)
	{
		// relative to the channel gains auto white balance last settled on (0x01-0x03 hold them)
		SccbWrite(0x43, (uint8_t)std::clamp(s.redBalance, 0, 255));
		SccbWrite(0x42, (uint8_t)std::clamp(s.blueBalance, 0, 255));
		SccbWrite(0x44, (uint8_t)std::clamp(s.greenBalance, 0, 255));
	}

	// the sensor is mounted mirrored: the bits *un*-flip the image
	uint8_t v = SccbRead(0x0c) & ~0xc0;
	if (!s.flipH)
		v |= 0x40;
	if (!s.flipV)
		v |= 0x80;
	SccbWrite(0x0c, v);
}

} // namespace ps3eye
