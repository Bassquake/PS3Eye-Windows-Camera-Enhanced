#include "pch.h"
#include "Undocumented.h"
#include "Tools.h"
#include "EnumNames.h"
#include "MFTools.h"
#include "MediaStream.h"
#include "MediaSource.h"
#include "Config.h"
#include "../core/convert.h"
#include "../core/ps3eye_camera.h"

namespace {

struct Mode
{
	const GUID* subtype;
	UINT32 width;
	UINT32 height;
	UINT32 fps;
};

// Index 0 is the default. NV12 first: it's what most capture pipelines (Zoom, Teams,
// Discord, browsers, Windows Camera) ask for. 640x480 is the sensor's full resolution.
const Mode kModes[] = {
	{ &MFVideoFormat_NV12, 640, 480, 30 },
	{ &MFVideoFormat_NV12, 640, 480, 60 },
	{ &MFVideoFormat_YUY2, 640, 480, 30 },
	{ &MFVideoFormat_YUY2, 640, 480, 60 },
	{ &MFVideoFormat_RGB32, 640, 480, 30 },
	{ &MFVideoFormat_RGB32, 640, 480, 60 },
	{ &MFVideoFormat_NV12, 320, 240, 30 },
	{ &MFVideoFormat_NV12, 320, 240, 60 },
	{ &MFVideoFormat_NV12, 320, 240, 120 },
	{ &MFVideoFormat_NV12, 320, 240, 187 },
	{ &MFVideoFormat_YUY2, 320, 240, 30 },
	{ &MFVideoFormat_YUY2, 320, 240, 60 },
	{ &MFVideoFormat_RGB32, 320, 240, 30 },
	{ &MFVideoFormat_RGB32, 320, 240, 60 },
};

UINT32 BytesPerFrame(const GUID& subtype, UINT32 w, UINT32 h)
{
	if (subtype == MFVideoFormat_NV12)
		return w * h * 3 / 2;
	if (subtype == MFVideoFormat_YUY2)
		return w * h * 2;
	return w * h * 4;
}

LONG DefaultStride(const GUID& subtype, UINT32 w)
{
	if (subtype == MFVideoFormat_NV12)
		return (LONG)w;
	if (subtype == MFVideoFormat_YUY2)
		return (LONG)w * 2;
	return (LONG)w * 4;
}

HRESULT CreateMode(const Mode& m, IMFMediaType** result)
{
	wil::com_ptr_nothrow<IMFMediaType> type;
	RETURN_IF_FAILED(MFCreateMediaType(&type));
	RETURN_IF_FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	RETURN_IF_FAILED(type->SetGUID(MF_MT_SUBTYPE, *m.subtype));
	RETURN_IF_FAILED(MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, m.width, m.height));
	RETURN_IF_FAILED(MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, m.fps, 1));
	RETURN_IF_FAILED(MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE_RANGE_MAX, m.fps, 1));
	RETURN_IF_FAILED(MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE_RANGE_MIN, m.fps, 1));
	RETURN_IF_FAILED(MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
	RETURN_IF_FAILED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	RETURN_IF_FAILED(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
	RETURN_IF_FAILED(type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
	RETURN_IF_FAILED(type->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)DefaultStride(*m.subtype, m.width)));
	UINT32 size = BytesPerFrame(*m.subtype, m.width, m.height);
	RETURN_IF_FAILED(type->SetUINT32(MF_MT_SAMPLE_SIZE, size));
	RETURN_IF_FAILED(type->SetUINT32(MF_MT_AVG_BITRATE, size * 8 * m.fps));
	if (*m.subtype != MFVideoFormat_RGB32)
	{
		RETURN_IF_FAILED(type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601));
		RETURN_IF_FAILED(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
	}
	*result = type.detach();
	return S_OK;
}

const wchar_t* FormatName(const GUID& f)
{
	if (f == MFVideoFormat_NV12) return L"NV12";
	if (f == MFVideoFormat_YUY2) return L"YUY2";
	if (f == MFVideoFormat_RGB32) return L"RGB32";
	return L"?";
}

} // namespace

HRESULT MediaStream::Initialize(IMFMediaSource* source, int index)
{
	RETURN_HR_IF_NULL(E_POINTER, source);
	_source = source;
	_index = index;

	RETURN_IF_FAILED(SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_STREAM_ID, index));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes::MFFrameSourceTypes_Color));

	RETURN_IF_FAILED(MFCreateEventQueue(&_queue));

	auto types = wil::make_unique_cotaskmem_array<wil::com_ptr_nothrow<IMFMediaType>>(std::size(kModes));
	for (size_t i = 0; i < std::size(kModes); i++)
	{
		wil::com_ptr_nothrow<IMFMediaType> type;
		RETURN_IF_FAILED(CreateMode(kModes[i], &type));
		types[i] = type.detach();
	}

	RETURN_IF_FAILED_MSG(MFCreateStreamDescriptor(_index, (DWORD)types.size(), types.get(), &_descriptor), "MFCreateStreamDescriptor failed");

	wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
	RETURN_IF_FAILED(_descriptor->GetMediaTypeHandler(&handler));
	RETURN_IF_FAILED(handler->SetCurrentMediaType(types[0]));
	return S_OK;
}

HRESULT MediaStream::Start(IMFMediaType* type)
{
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_allocator);

	// stop any previous capture before touching the format fields it reads
	StopCapture();

	if (type)
	{
		_currentType = type;
	}
	else if (!_currentType)
	{
		wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
		RETURN_IF_FAILED(_descriptor->GetMediaTypeHandler(&handler));
		RETURN_IF_FAILED(handler->GetCurrentMediaType(&_currentType));
	}

	RETURN_IF_FAILED(_currentType->GetGUID(MF_MT_SUBTYPE, &_format));
	RETURN_IF_FAILED(MFGetAttributeSize(_currentType.get(), MF_MT_FRAME_SIZE, &_width, &_height));
	UINT32 num = 30, den = 1;
	if (SUCCEEDED(MFGetAttributeRatio(_currentType.get(), MF_MT_FRAME_RATE, &num, &den)) && den)
		_fps = std::max<UINT32>(1, num / den);
	RETURN_HR_IF(MF_E_INVALIDMEDIATYPE, _format != MFVideoFormat_NV12 && _format != MFVideoFormat_YUY2 && _format != MFVideoFormat_RGB32);
	RETURN_HR_IF(MF_E_INVALIDMEDIATYPE, !((_width == 640 && _height == 480) || (_width == 320 && _height == 240)));

	vcam::Log(L"Stream start: %s %ux%u @ %u fps", FormatName(_format), _width, _height, _fps);

	RETURN_IF_FAILED(_allocator->InitializeSampleAllocator(10, _currentType.get()));
	{
		winrt::slim_lock_guard lock(_lock);
		_requests.clear();
		_state = MF_STREAM_STATE_RUNNING;
	}
	StartCapture();

	RETURN_IF_FAILED(_queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
	return S_OK;
}

HRESULT MediaStream::Stop()
{
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_allocator);

	StopCapture(); // never hold _lock while joining: the capture thread takes it
	{
		winrt::slim_lock_guard lock(_lock);
		_requests.clear();
		_state = MF_STREAM_STATE_STOPPED;
	}
	RETURN_IF_FAILED(_allocator->UninitializeSampleAllocator());
	RETURN_IF_FAILED(_queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr));
	vcam::Log(L"Stream stopped");
	return S_OK;
}

MFSampleAllocatorUsage MediaStream::GetAllocatorUsage()
{
	return MFSampleAllocatorUsage_UsesProvidedAllocator;
}

HRESULT MediaStream::SetAllocator(IUnknown* allocator)
{
	RETURN_HR_IF_NULL(E_POINTER, allocator);
	_allocator.reset();
	RETURN_HR(allocator->QueryInterface(&_allocator));
}

HRESULT MediaStream::SetD3DManager(IUnknown* manager)
{
	// Frames are produced on the CPU (USB -> demosaic -> YUV), so system-memory samples are the
	// cheapest path; we deliberately don't hand the D3D manager to the allocator.
	RETURN_HR_IF_NULL(E_POINTER, manager);
	return S_OK;
}

void MediaStream::Shutdown()
{
	StopCapture();
	{
		winrt::slim_lock_guard lock(_lock);
		_requests.clear();
	}

	if (_queue)
	{
		LOG_IF_FAILED_MSG(_queue->Shutdown(), "Queue shutdown failed");
		_queue.reset();
	}

	_descriptor.reset();
	_source.reset();
	_currentType.reset();
	_attributes.reset();
}

// ---- capture ---------------------------------------------------------------

void MediaStream::StartCapture()
{
	_quit = false;
	_thread = std::thread(&MediaStream::CaptureLoop, this);
}

void MediaStream::StopCapture()
{
	_quit = true;
	if (_thread.joinable())
	{
		if (_thread.get_id() == std::this_thread::get_id())
			_thread.detach(); // should never happen, but never self-join
		else
			_thread.join();
	}
}

void MediaStream::CaptureLoop()
{
	using namespace std::chrono;
	using Clock = steady_clock;

	SetThreadDescription(GetCurrentThread(), L"PS3 Eye capture");
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

	const UINT32 w = _width, h = _height, fps = _fps;
	std::vector<uint8_t> bayer;
	std::vector<uint8_t> bgrx((size_t)w * h * 4);

	vcam::Config config = vcam::LoadConfig();
	ps3eye::Camera camera;
	ps3eye::Status lastStatus = ps3eye::Status::Ok; // only log a failure when it changes

	auto lastAttempt = Clock::now() - seconds(10);
	auto lastFrame = Clock::now();
	auto lastPlaceholder = Clock::now() - seconds(10);
	uint64_t frames = 0;
	auto statsStart = Clock::now();

	// live settings: re-read the registry key whenever it changes
	HKEY key = nullptr;
	HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	auto watchSettings = [&]
	{
		if (!key)
			RegCreateKeyExW(HKEY_LOCAL_MACHINE, vcam::kRegKey, 0, nullptr, 0, KEY_NOTIFY | KEY_READ, nullptr, &key, nullptr);
		if (!key)
			RegOpenKeyExW(HKEY_LOCAL_MACHINE, vcam::kRegKey, 0, KEY_NOTIFY | KEY_READ, &key);
		if (key && changed)
			RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC, changed, TRUE);
	};
	watchSettings();

	while (!_quit)
	{
		// (re)connect
		if (!camera.IsStreaming() || camera.DeviceLost())
		{
			if (camera.IsStreaming())
			{
				vcam::Log(L"Camera lost (unplugged or USB error); retrying every second");
				camera.Stop();
			}

			if (Clock::now() - lastAttempt >= seconds(1))
			{
				lastAttempt = Clock::now();
				ps3eye::Status st = camera.Start(w, h, fps, config.camera, config.deviceIndex);
				if (st == ps3eye::Status::Ok)
				{
					vcam::Log(L"Camera streaming %ux%u @ %u fps (requested %u)", camera.Width(), camera.Height(), camera.Fps(), fps);
					lastFrame = Clock::now();
				}
				else if (st != lastStatus)
				{
					vcam::Log(L"Camera start failed: %S", ps3eye::StatusText(st));
				}
				lastStatus = st;
			}
		}

		// frames
		if (camera.IsStreaming() && !camera.DeviceLost())
		{
			if (camera.WaitFrame(bayer, milliseconds(100)))
			{
				auto time = MFGetSystemTime();
				ps3eye::BayerToBGRX(bayer.data(), (int)w, (int)h, bgrx.data(), (long)w * 4);
				Deliver(bgrx.data(), time);
				lastFrame = Clock::now();

				frames++;
				if (Clock::now() - statsStart >= minutes(5))
				{
					double secs = duration<double>(Clock::now() - statsStart).count();
					vcam::Log(L"Camera running at %.1f fps", frames / secs);
					statsStart = Clock::now();
					frames = 0;
				}
			}
			else if (Clock::now() - lastFrame > seconds(3))
			{
				vcam::Log(L"No frames for 3 s; restarting the camera");
				camera.Stop();
			}
		}
		else
		{
			// no camera: keep apps alive with a placeholder at 5 fps
			if (Clock::now() - lastPlaceholder >= milliseconds(200))
			{
				lastPlaceholder = Clock::now();
				ps3eye::DrawNoSignal(bgrx.data(), (int)w, (int)h, (long)w * 4);
				Deliver(bgrx.data(), MFGetSystemTime());
			}
			std::this_thread::sleep_for(milliseconds(20));
		}

		// settings changed?
		if (changed && WaitForSingleObject(changed, 0) == WAIT_OBJECT_0)
		{
			vcam::Config next = vcam::LoadConfig();
			vcam::Log(L"Settings changed; applying");
			if (next.deviceIndex != config.deviceIndex)
				camera.Stop(); // reconnect to the other camera
			else
				camera.Apply(next.camera);
			config = next;
			watchSettings();
		}
	}

	camera.Stop();
	if (key)
		RegCloseKey(key);
	if (changed)
		CloseHandle(changed);
}

void MediaStream::Deliver(const uint8_t* bgrx, MFTIME time)
{
	winrt::slim_lock_guard lock(_lock);
	if (_state != MF_STREAM_STATE_RUNNING || _requests.empty() || !_allocator || !_queue)
		return; // nobody asked: drop the frame (the next one is newer anyway)

	wil::com_ptr_nothrow<IMFSample> sample;
	if (FAILED(_allocator->AllocateSample(&sample)))
		return; // all samples in flight; try again with the next frame

	if (FAILED(sample->SetSampleTime(time)) ||
		FAILED(sample->SetSampleDuration(10000000LL / std::max<UINT32>(1, _fps))) ||
		FAILED(FillSample(sample.get(), bgrx)))
		return;

	auto token = std::move(_requests.front());
	_requests.pop_front();
	if (token)
		LOG_IF_FAILED(sample->SetUnknown(MFSampleExtension_Token, token.get()));

	LOG_IF_FAILED(_queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.get()));
}

HRESULT MediaStream::FillSample(IMFSample* sample, const uint8_t* bgrx)
{
	wil::com_ptr_nothrow<IMFMediaBuffer> buffer;
	RETURN_IF_FAILED(sample->GetBufferByIndex(0, &buffer));

	const int w = (int)_width, h = (int)_height;
	BYTE* scanline = nullptr;
	LONG pitch = 0;
	DWORD capacity = 0;

	wil::com_ptr_nothrow<IMF2DBuffer2> buffer2D;
	if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2D))))
	{
		BYTE* start = nullptr;
		RETURN_IF_FAILED(buffer2D->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &pitch, &start, &capacity));
	}
	else
	{
		DWORD max = 0;
		RETURN_IF_FAILED(buffer->Lock(&scanline, &max, nullptr));
		capacity = max;
		pitch = DefaultStride(_format, _width);
	}

	const DWORD needed = BytesPerFrame(_format, _width, _height);
	const DWORD rows = _format == MFVideoFormat_NV12 ? (DWORD)h * 3 / 2 : (DWORD)h;
	const DWORD required = (DWORD)std::abs(pitch) * rows;
	HRESULT hr = S_OK;
	if (!scanline || capacity < required || (_format == MFVideoFormat_NV12 && pitch < 0))
	{
		hr = MF_E_BUFFERTOOSMALL;
	}
	else if (_format == MFVideoFormat_NV12)
	{
		ps3eye::BGRXToNV12(bgrx, w, h, (long)w * 4, scanline, pitch);
	}
	else if (_format == MFVideoFormat_YUY2)
	{
		ps3eye::BGRXToYUY2(bgrx, w, h, (long)w * 4, scanline, pitch);
	}
	else
	{
		ps3eye::CopyBGRX(bgrx, w, h, (long)w * 4, scanline, pitch);
	}

	if (buffer2D)
		buffer2D->Unlock2D();
	else
		buffer->Unlock();

	RETURN_IF_FAILED(hr);
	RETURN_IF_FAILED(buffer->SetCurrentLength((std::min)(capacity, (std::max)(needed, required))));
	return S_OK;
}

// IMFMediaEventGenerator
STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->BeginGetEvent(pCallback, punkState));
	return S_OK;
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->EndGetEvent(pResult, ppEvent));
	return S_OK;
}

STDMETHODIMP MediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	wil::com_ptr_nothrow<IMFMediaEventQueue> queue;
	{
		winrt::slim_lock_guard lock(_lock);
		RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
		queue = _queue;
	}
	// GetEvent can block: don't hold the lock while waiting or Deliver() would stall
	RETURN_IF_FAILED(queue->GetEvent(dwFlags, ppEvent));
	return S_OK;
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue));
	return S_OK;
}

// IMFMediaStream
STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** ppMediaSource)
{
	RETURN_HR_IF_NULL(E_POINTER, ppMediaSource);
	*ppMediaSource = nullptr;
	RETURN_HR_IF(MF_E_SHUTDOWN, !_source);

	RETURN_IF_FAILED(_source.copy_to(ppMediaSource));
	return S_OK;
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor)
{
	RETURN_HR_IF_NULL(E_POINTER, ppStreamDescriptor);
	*ppStreamDescriptor = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_descriptor);

	RETURN_IF_FAILED(_descriptor.copy_to(ppStreamDescriptor));
	return S_OK;
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* pToken)
{
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_allocator || !_queue);
	RETURN_HR_IF(MF_E_INVALIDREQUEST, _state == MF_STREAM_STATE_STOPPED);

	// answered by the capture thread when the next frame is ready
	wil::com_ptr_nothrow<IUnknown> token(pToken);
	_requests.push_back(std::move(token));
	if (_requests.size() > 30)
		_requests.pop_front(); // a client that never reads shouldn't grow this forever
	return S_OK;
}

// IMFMediaStream2
STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE value)
{
	WINTRACE(L"MediaStream::SetStreamState current:%u value:%u", _state, value);
	if (_state == value)
		return S_OK;

	switch (value)
	{
	case MF_STREAM_STATE_PAUSED:
		if (_state != MF_STREAM_STATE_RUNNING)
			RETURN_HR(MF_E_INVALID_STATE_TRANSITION);
		{
			winrt::slim_lock_guard lock(_lock);
			_state = value; // capture keeps running, frames are dropped while paused
		}
		break;

	case MF_STREAM_STATE_RUNNING:
		if (_state == MF_STREAM_STATE_PAUSED)
		{
			winrt::slim_lock_guard lock(_lock);
			_state = value;
		}
		else
		{
			RETURN_IF_FAILED(Start(nullptr));
		}
		break;

	case MF_STREAM_STATE_STOPPED:
		RETURN_IF_FAILED(Stop());
		break;

	default:
		RETURN_HR(MF_E_INVALID_STATE_TRANSITION);
	}
	return S_OK;
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* value)
{
	RETURN_HR_IF_NULL(E_POINTER, value);
	*value = _state;
	return S_OK;
}

// IKsControl
STDMETHODIMP_(NTSTATUS) MediaStream::KsProperty(PKSPROPERTY property, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	RETURN_HR_IF_NULL(E_POINTER, property);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	UNREFERENCED_PARAMETER(length);
	UNREFERENCED_PARAMETER(data);
	UNREFERENCED_PARAMETER(dataLength);
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaStream::KsMethod(PKSMETHOD method, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	RETURN_HR_IF_NULL(E_POINTER, method);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	UNREFERENCED_PARAMETER(length);
	UNREFERENCED_PARAMETER(data);
	UNREFERENCED_PARAMETER(dataLength);
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaStream::KsEvent(PKSEVENT evt, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	UNREFERENCED_PARAMETER(evt);
	UNREFERENCED_PARAMETER(length);
	UNREFERENCED_PARAMETER(data);
	UNREFERENCED_PARAMETER(dataLength);
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
