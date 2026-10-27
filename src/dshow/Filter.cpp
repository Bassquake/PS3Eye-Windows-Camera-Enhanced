#include "Filter.h"

#include <ks.h>
#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "../core/convert.h"
#include "../core/rates.h"
#include "../source/Config.h"
#include "../source/Controls.h"
#include "SharedFrames.h"

namespace vcam {

namespace {

const wchar_t* kPinName = L"Capture";

struct Lock
{
	explicit Lock(CRITICAL_SECTION& cs) : _cs(cs) { EnterCriticalSection(&_cs); }
	~Lock() { LeaveCriticalSection(&_cs); }
	Lock(const Lock&) = delete;
	Lock& operator=(const Lock&) = delete;
	CRITICAL_SECTION& _cs;
};

REFERENCE_TIME SteadyNow()
{
	static LARGE_INTEGER freq = []
	{
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return f;
	}();
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (REFERENCE_TIME)(now.QuadPart / freq.QuadPart * 10000000LL + (now.QuadPart % freq.QuadPart) * 10000000LL / freq.QuadPart);
}

std::wstring ModuleDir()
{
	wchar_t path[MAX_PATH] = L"";
	GetModuleFileNameW(gModule, path, MAX_PATH);
	std::wstring s = path;
	size_t slash = s.find_last_of(L'\\');
	return slash == std::wstring::npos ? s : s.substr(0, slash);
}

// ---- enumerators ---------------------------------------------------------------------

class PinEnum final : public IEnumPins
{
public:
	PinEnum(IBaseFilter* filter, IPin* pin, ULONG pos) : _filter(filter), _pin(pin), _pos(pos)
	{
		_filter->AddRef();
		gObjects++;
	}
	~PinEnum()
	{
		_filter->Release();
		gObjects--;
	}

	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
	{
		if (!ppv)
			return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IEnumPins)
		{
			*ppv = static_cast<IEnumPins*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return ++_refs; }
	STDMETHODIMP_(ULONG) Release() override
	{
		ULONG n = --_refs;
		if (!n)
			delete this;
		return n;
	}

	STDMETHODIMP Next(ULONG count, IPin** pins, ULONG* fetched) override
	{
		if (!pins || (count > 1 && !fetched))
			return E_POINTER;
		ULONG n = 0;
		if (count > 0 && _pos == 0)
		{
			pins[0] = _pin;
			_pin->AddRef();
			_pos = 1;
			n = 1;
		}
		if (fetched)
			*fetched = n;
		return n == count ? S_OK : S_FALSE;
	}
	STDMETHODIMP Skip(ULONG count) override
	{
		_pos = std::min<ULONG>(1, _pos + count);
		return _pos == 1 && count > 1 ? S_FALSE : S_OK;
	}
	STDMETHODIMP Reset() override
	{
		_pos = 0;
		return S_OK;
	}
	STDMETHODIMP Clone(IEnumPins** ppEnum) override
	{
		if (!ppEnum)
			return E_POINTER;
		*ppEnum = new (std::nothrow) PinEnum(_filter, _pin, _pos);
		return *ppEnum ? S_OK : E_OUTOFMEMORY;
	}

private:
	std::atomic<ULONG> _refs{ 1 };
	IBaseFilter* _filter;
	IPin* _pin;
	ULONG _pos;
};

class MediaTypeEnum final : public IEnumMediaTypes
{
public:
	MediaTypeEnum() { gObjects++; }
	~MediaTypeEnum()
	{
		for (AM_MEDIA_TYPE* mt : _types)
			DeleteMediaType(mt);
		gObjects--;
	}

	bool Add(const AM_MEDIA_TYPE& mt)
	{
		AM_MEDIA_TYPE* copy = CreateMediaType(&mt);
		if (copy)
			_types.push_back(copy);
		return copy != nullptr;
	}

	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
	{
		if (!ppv)
			return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes)
		{
			*ppv = static_cast<IEnumMediaTypes*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return ++_refs; }
	STDMETHODIMP_(ULONG) Release() override
	{
		ULONG n = --_refs;
		if (!n)
			delete this;
		return n;
	}

	STDMETHODIMP Next(ULONG count, AM_MEDIA_TYPE** types, ULONG* fetched) override
	{
		if (!types || (count > 1 && !fetched))
			return E_POINTER;
		ULONG n = 0;
		while (n < count && _pos < _types.size())
		{
			types[n] = CreateMediaType(_types[_pos]);
			if (!types[n])
				break;
			n++;
			_pos++;
		}
		if (fetched)
			*fetched = n;
		return n == count ? S_OK : S_FALSE;
	}
	STDMETHODIMP Skip(ULONG count) override
	{
		_pos += count;
		if (_pos > _types.size())
		{
			_pos = _types.size();
			return S_FALSE;
		}
		return S_OK;
	}
	STDMETHODIMP Reset() override
	{
		_pos = 0;
		return S_OK;
	}
	STDMETHODIMP Clone(IEnumMediaTypes** ppEnum) override
	{
		if (!ppEnum)
			return E_POINTER;
		MediaTypeEnum* e = new (std::nothrow) MediaTypeEnum();
		if (!e)
			return E_OUTOFMEMORY;
		for (AM_MEDIA_TYPE* mt : _types)
			e->Add(*mt);
		e->_pos = _pos;
		*ppEnum = e;
		return S_OK;
	}

private:
	std::atomic<ULONG> _refs{ 1 };
	std::vector<AM_MEDIA_TYPE*> _types;
	size_t _pos = 0;
};

} // namespace

// ==== Pin ==================================================================================

Pin::Pin(Filter* filter) : _filter(filter)
{
}

Pin::~Pin()
{
	Inactive();
	ReleaseConnection();
	DeleteMediaType(_preferred);
}

STDMETHODIMP Pin::QueryInterface(REFIID riid, void** ppv)
{
	if (!ppv)
		return E_POINTER;
	if (riid == IID_IUnknown || riid == IID_IPin)
		*ppv = static_cast<IPin*>(this);
	else if (riid == IID_IAMStreamConfig)
		*ppv = static_cast<IAMStreamConfig*>(this);
	else if (riid == IID_IKsPropertySet)
		*ppv = static_cast<IKsPropertySet*>(this);
	else
	{
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	AddRef();
	return S_OK;
}

STDMETHODIMP_(ULONG) Pin::AddRef() { return _filter->AddRef(); }
STDMETHODIMP_(ULONG) Pin::Release() { return _filter->Release(); }

void Pin::ReleaseConnection()
{
	if (_allocator)
	{
		_allocator->Decommit();
		_allocator->Release();
		_allocator = nullptr;
	}
	if (_input)
	{
		_input->Release();
		_input = nullptr;
	}
	if (_connected)
	{
		_connected->Release();
		_connected = nullptr;
	}
	FreeMediaType(_mt);
	memset(&_mt, 0, sizeof(_mt));
}

HRESULT Pin::DecideAllocator(IMemInputPin* input, const StreamFormat& format)
{
	ALLOCATOR_PROPERTIES wanted{};
	input->GetAllocatorRequirements(&wanted); // optional; zeros if not implemented
	wanted.cBuffers = std::max<long>(wanted.cBuffers, 3);
	wanted.cbBuffer = std::max<long>(wanted.cbBuffer, (long)format.size);
	wanted.cbAlign = std::max<long>(wanted.cbAlign, 1);

	auto tryAllocator = [&](IMemAllocator* alloc) -> HRESULT
	{
		ALLOCATOR_PROPERTIES actual{};
		HRESULT hr = alloc->SetProperties(&wanted, &actual);
		if (FAILED(hr))
			return hr;
		if (actual.cbBuffer < (long)format.size)
			return E_FAIL;
		return input->NotifyAllocator(alloc, FALSE);
	};

	IMemAllocator* alloc = nullptr;
	if (SUCCEEDED(input->GetAllocator(&alloc)) && alloc)
	{
		if (SUCCEEDED(tryAllocator(alloc)))
		{
			_allocator = alloc;
			return S_OK;
		}
		alloc->Release();
		alloc = nullptr;
	}

	HRESULT hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator, (void**)&alloc);
	if (FAILED(hr))
		return hr;
	hr = tryAllocator(alloc);
	if (FAILED(hr))
	{
		alloc->Release();
		return hr;
	}
	_allocator = alloc;
	return S_OK;
}

HRESULT Pin::TryConnect(IPin* receivePin, const AM_MEDIA_TYPE& mt)
{
	StreamFormat format;
	if (!DescribeFormat(mt, format))
		return VFW_E_TYPE_NOT_ACCEPTED;

	HRESULT hr = receivePin->ReceiveConnection(this, &mt);
	if (FAILED(hr))
		return hr;

	IMemInputPin* input = nullptr;
	if (FAILED(receivePin->QueryInterface(IID_IMemInputPin, (void**)&input)) || !input)
	{
		receivePin->Disconnect();
		return VFW_E_NO_TRANSPORT;
	}
	hr = DecideAllocator(input, format);
	if (FAILED(hr))
	{
		input->Release();
		receivePin->Disconnect();
		return hr;
	}
	if (FAILED(CopyMediaType(&_mt, &mt)))
	{
		ReleaseConnection();
		input->Release();
		receivePin->Disconnect();
		return E_OUTOFMEMORY;
	}

	_connected = receivePin;
	_connected->AddRef();
	_input = input;
	Log(L"DirectShow: connected %s %dx%d @ %u fps", SubtypeName(format.subtype), format.width, format.height, format.fps);
	return S_OK;
}

STDMETHODIMP Pin::Connect(IPin* receivePin, const AM_MEDIA_TYPE* pmt)
{
	if (!receivePin)
		return E_POINTER;
	Lock lock(_filter->_lock);
	if (_connected)
		return VFW_E_ALREADY_CONNECTED;
	if (_filter->_state != State_Stopped)
		return VFW_E_NOT_STOPPED;

	// a fully specified type: that one or nothing
	if (pmt && pmt->majortype != GUID_NULL && pmt->subtype != GUID_NULL && pmt->formattype != GUID_NULL && pmt->pbFormat)
		return TryConnect(receivePin, *pmt);

	auto fits = [&](const AM_MEDIA_TYPE& mt)
	{
		if (!pmt)
			return true;
		return (pmt->majortype == GUID_NULL || pmt->majortype == mt.majortype) && (pmt->subtype == GUID_NULL || pmt->subtype == mt.subtype);
	};

	// the format picked with IAMStreamConfig::SetFormat goes first
	if (_preferred && fits(*_preferred) && SUCCEEDED(TryConnect(receivePin, *_preferred)))
		return S_OK;

	size_t n = 0;
	const VideoFormat* formats = Formats(&n);
	for (size_t i = 0; i < n; i++)
	{
		AM_MEDIA_TYPE mt;
		if (FAILED(BuildMediaType(formats[i], formats[i].fps, mt)))
			return E_OUTOFMEMORY;
		HRESULT hr = fits(mt) ? TryConnect(receivePin, mt) : E_FAIL;
		FreeMediaType(mt);
		if (SUCCEEDED(hr))
			return S_OK;
	}
	return VFW_E_NO_ACCEPTABLE_TYPES;
}

STDMETHODIMP Pin::ReceiveConnection(IPin*, const AM_MEDIA_TYPE*)
{
	return E_UNEXPECTED; // output pin
}

STDMETHODIMP Pin::Disconnect()
{
	Lock lock(_filter->_lock);
	if (_filter->_state != State_Stopped)
		return VFW_E_NOT_STOPPED;
	if (!_connected)
		return S_FALSE;
	ReleaseConnection();
	return S_OK;
}

STDMETHODIMP Pin::ConnectedTo(IPin** pin)
{
	if (!pin)
		return E_POINTER;
	Lock lock(_filter->_lock);
	*pin = _connected;
	if (!_connected)
		return VFW_E_NOT_CONNECTED;
	_connected->AddRef();
	return S_OK;
}

STDMETHODIMP Pin::ConnectionMediaType(AM_MEDIA_TYPE* pmt)
{
	if (!pmt)
		return E_POINTER;
	Lock lock(_filter->_lock);
	if (!_connected)
	{
		memset(pmt, 0, sizeof(*pmt));
		return VFW_E_NOT_CONNECTED;
	}
	return CopyMediaType(pmt, &_mt);
}

STDMETHODIMP Pin::QueryPinInfo(PIN_INFO* info)
{
	if (!info)
		return E_POINTER;
	info->pFilter = _filter;
	_filter->AddRef();
	info->dir = PINDIR_OUTPUT;
	wcscpy_s(info->achName, kPinName);
	return S_OK;
}

STDMETHODIMP Pin::QueryDirection(PIN_DIRECTION* dir)
{
	if (!dir)
		return E_POINTER;
	*dir = PINDIR_OUTPUT;
	return S_OK;
}

STDMETHODIMP Pin::QueryId(LPWSTR* id)
{
	if (!id)
		return E_POINTER;
	size_t bytes = (wcslen(kPinName) + 1) * sizeof(wchar_t);
	*id = (LPWSTR)CoTaskMemAlloc(bytes);
	if (!*id)
		return E_OUTOFMEMORY;
	memcpy(*id, kPinName, bytes);
	return S_OK;
}

STDMETHODIMP Pin::QueryAccept(const AM_MEDIA_TYPE* pmt)
{
	if (!pmt)
		return E_POINTER;
	StreamFormat f;
	return DescribeFormat(*pmt, f) ? S_OK : S_FALSE;
}

STDMETHODIMP Pin::EnumMediaTypes(IEnumMediaTypes** ppEnum)
{
	if (!ppEnum)
		return E_POINTER;
	MediaTypeEnum* e = new (std::nothrow) MediaTypeEnum();
	if (!e)
		return E_OUTOFMEMORY;
	{
		Lock lock(_filter->_lock);
		if (_preferred)
			e->Add(*_preferred);
	}
	size_t n = 0;
	const VideoFormat* formats = Formats(&n);
	for (size_t i = 0; i < n; i++)
	{
		AM_MEDIA_TYPE mt;
		if (SUCCEEDED(BuildMediaType(formats[i], formats[i].fps, mt)))
		{
			e->Add(mt);
			FreeMediaType(mt);
		}
	}
	*ppEnum = e;
	return S_OK;
}

STDMETHODIMP Pin::QueryInternalConnections(IPin**, ULONG*) { return E_NOTIMPL; }
STDMETHODIMP Pin::EndOfStream() { return E_UNEXPECTED; }
STDMETHODIMP Pin::BeginFlush() { return E_UNEXPECTED; }
STDMETHODIMP Pin::EndFlush() { return E_UNEXPECTED; }
STDMETHODIMP Pin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

// ---- IAMStreamConfig ----

STDMETHODIMP Pin::SetFormat(AM_MEDIA_TYPE* pmt)
{
	if (!pmt)
		return E_POINTER;
	AM_MEDIA_TYPE normalized;
	if (FAILED(NormalizeMediaType(*pmt, normalized)))
		return VFW_E_INVALIDMEDIATYPE;

	bool reconnect = false;
	{
		Lock lock(_filter->_lock);
		if (_connected)
		{
			if (_filter->_state != State_Stopped)
			{
				FreeMediaType(normalized);
				return VFW_E_NOT_STOPPED;
			}
			if (_connected->QueryAccept(&normalized) != S_OK)
			{
				FreeMediaType(normalized);
				return VFW_E_INVALIDMEDIATYPE;
			}
			reconnect = true;
		}
		DeleteMediaType(_preferred);
		_preferred = CreateMediaType(&normalized);
	}
	FreeMediaType(normalized);

	// already connected: reconnect so the new format (tried first in Connect) takes effect
	IFilterGraph* graph = _filter->_graph;
	if (reconnect && graph)
	{
		IFilterGraph2* graph2 = nullptr;
		HRESULT hr;
		if (SUCCEEDED(graph->QueryInterface(IID_IFilterGraph2, (void**)&graph2)))
		{
			hr = graph2->ReconnectEx(this, nullptr);
			graph2->Release();
		}
		else
		{
			hr = graph->Reconnect(this);
		}
		return hr;
	}
	return S_OK;
}

STDMETHODIMP Pin::GetFormat(AM_MEDIA_TYPE** ppmt)
{
	if (!ppmt)
		return E_POINTER;
	Lock lock(_filter->_lock);
	if (_connected)
		*ppmt = CreateMediaType(&_mt);
	else if (_preferred)
		*ppmt = CreateMediaType(_preferred);
	else
	{
		size_t n = 0;
		const VideoFormat* formats = Formats(&n);
		AM_MEDIA_TYPE mt;
		if (FAILED(BuildMediaType(formats[0], formats[0].fps, mt)))
			return E_OUTOFMEMORY;
		*ppmt = CreateMediaType(&mt);
		FreeMediaType(mt);
	}
	return *ppmt ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP Pin::GetNumberOfCapabilities(int* count, int* size)
{
	if (!count || !size)
		return E_POINTER;
	size_t n = 0;
	Formats(&n);
	*count = (int)n;
	*size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
	return S_OK;
}

STDMETHODIMP Pin::GetStreamCaps(int index, AM_MEDIA_TYPE** ppmt, BYTE* scc)
{
	if (!ppmt || !scc)
		return E_POINTER;
	size_t n = 0;
	const VideoFormat* formats = Formats(&n);
	if (index < 0 || (size_t)index >= n)
		return S_FALSE;
	const VideoFormat& f = formats[index];

	AM_MEDIA_TYPE mt;
	if (FAILED(BuildMediaType(f, f.fps, mt)))
		return E_OUTOFMEMORY;
	*ppmt = CreateMediaType(&mt);
	const LONG frameBits = (LONG)((const VIDEOINFOHEADER*)mt.pbFormat)->bmiHeader.biSizeImage * 8;
	FreeMediaType(mt);
	if (!*ppmt)
		return E_OUTOFMEMORY;

	VIDEO_STREAM_CONFIG_CAPS* caps = (VIDEO_STREAM_CONFIG_CAPS*)scc;
	memset(caps, 0, sizeof(*caps));
	const SIZE size{ (LONG)f.width, (LONG)f.height };
	caps->guid = FORMAT_VideoInfo;
	caps->VideoStandard = AnalogVideo_None;
	caps->InputSize = size;
	caps->MinCroppingSize = size;
	caps->MaxCroppingSize = size;
	caps->CropGranularityX = 1;
	caps->CropGranularityY = 1;
	caps->CropAlignX = 1;
	caps->CropAlignY = 1;
	caps->MinOutputSize = size;
	caps->MaxOutputSize = size;
	caps->OutputGranularityX = 1;
	caps->OutputGranularityY = 1;
	caps->MinFrameInterval = 10000000LL / MaxFps(f.width);
	caps->MaxFrameInterval = 10000000LL; // 1 fps: lower rates are made by skipping frames
	caps->MinBitsPerSecond = frameBits;
	caps->MaxBitsPerSecond = frameBits * (LONG)MaxFps(f.width);
	return S_OK;
}

// ---- IKsPropertySet ----

STDMETHODIMP Pin::Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD)
{
	return E_PROP_SET_UNSUPPORTED;
}

STDMETHODIMP Pin::Get(REFGUID set, DWORD id, LPVOID, DWORD, LPVOID data, DWORD dataSize, DWORD* returned)
{
	if (set != AMPROPSETID_Pin)
		return E_PROP_SET_UNSUPPORTED;
	if (id != AMPROPERTY_PIN_CATEGORY)
		return E_PROP_ID_UNSUPPORTED;
	if (!data && !returned)
		return E_POINTER;
	if (returned)
		*returned = sizeof(GUID);
	if (!data)
		return S_OK;
	if (dataSize < sizeof(GUID))
		return E_UNEXPECTED;
	*(GUID*)data = PIN_CATEGORY_CAPTURE;
	return S_OK;
}

STDMETHODIMP Pin::QuerySupported(REFGUID set, DWORD id, DWORD* support)
{
	if (set != AMPROPSETID_Pin)
		return E_PROP_SET_UNSUPPORTED;
	if (id != AMPROPERTY_PIN_CATEGORY)
		return E_PROP_ID_UNSUPPORTED;
	if (support)
		*support = KSPROPERTY_SUPPORT_GET;
	return S_OK;
}

// ---- streaming ----

HRESULT Pin::Active()
{
	if (!_connected || !_allocator)
		return S_OK;
	StreamFormat format;
	if (!DescribeFormat(_mt, format))
		return VFW_E_INVALIDMEDIATYPE;
	HRESULT hr = _allocator->Commit();
	if (FAILED(hr))
		return hr;
	_quit = false;
	_thread = std::thread(&Pin::Stream, this, format);
	return S_OK;
}

void Pin::Inactive()
{
	_quit = true;
	if (_allocator)
		_allocator->Decommit(); // wakes a GetBuffer() waiting for a free sample
	if (_thread.joinable())
	{
		if (_thread.get_id() == std::this_thread::get_id())
			_thread.detach(); // should never happen, but never self-join
		else
			_thread.join();
	}
}

void Pin::Stream(StreamFormat format)
{
	HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	SetThreadDescription(GetCurrentThread(), L"PS3 Eye DirectShow");
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	Log(L"DirectShow: stream start %s %dx%d @ %u fps", SubtypeName(format.subtype), format.width, format.height, format.fps);

	const std::wstring dir = ModuleDir();
	SharedFrames shared;
	std::vector<uint8_t> bgrx(kSharedFrameBytes), scratch(kSharedFrameBytes);
	int srcWidth = (int)kSharedWidth, srcHeight = (int)kSharedHeight;
	uint64_t seq = 0;
	bool first = true, launchFailed = false;
	ULONGLONG lastFrame = GetTickCount64(), lastLaunch = 0, lastPlaceholder = 0;
	const REFERENCE_TIME interval = 10000000LL / std::max<uint32_t>(1, format.fps);
	const REFERENCE_TIME begin = SteadyNow();
	REFERENCE_TIME lastSent = begin - interval, nextDue = begin;

	while (!_quit)
	{
		if (!shared.IsOpen())
			shared.Open();
		if (shared.IsOpen())
		{
			if (shared.Claim(format.fps, (uint32_t)format.width) < 0)
				Sleep(10); // all slots taken; frames are still readable
			shared.Refresh();
		}

		ULONGLONG tick = GetTickCount64();
		if ((seq == 0 || tick - lastFrame > 1000) && tick - lastLaunch > 3000 && !SharedFrames::ServerRunning())
		{
			lastLaunch = tick;
			if (SharedFrames::StartServer(dir.c_str()))
				launchFailed = false;
			else if (!launchFailed)
			{
				launchFailed = true;
				Log(L"DirectShow: can't start %s\\%s (error %lu)", dir.c_str(), kServerExe, GetLastError());
			}
		}

		HANDLE ev = shared.FrameEvent();
		if (ev)
			WaitForSingleObject(ev, 100);
		else
			Sleep(20);
		if (_quit)
			break;

		bool real = false, got = shared.ReadLatest(seq, bgrx.data(), srcWidth, srcHeight, &real);
		tick = GetTickCount64();
		if (got)
			lastFrame = tick;
		else if (tick - lastFrame > 2000 && tick - lastPlaceholder >= 200)
		{
			// server not delivering (not started yet, or gone): keep the app alive
			lastPlaceholder = tick;
			srcWidth = (int)kSharedWidth;
			srcHeight = (int)kSharedHeight;
			ps3eye::DrawNoSignal(bgrx.data(), srcWidth, srcHeight, (long)srcWidth * 4);
			got = true;
		}
		if (!got || !_filter->_running)
			continue; // paused: live sources don't deliver

		// the camera may run faster than this app asked for (another app wants 75 fps): send on a
		// fixed schedule, taking the frame nearest each slot, so e.g. 30 out of 75 averages 30
		const REFERENCE_TIME now = SteadyNow();
		if (now < nextDue - interval / 2)
			continue;
		nextDue = now - nextDue > interval ? now + interval : nextDue + interval; // fell behind: resync
		// smoothed, for IAMVideoControl::GetCurrentActualFrameRate
		const LONGLONG prev = _actualInterval, gap = std::min<LONGLONG>(now - lastSent, 10000000LL);
		_actualInterval = first || !prev ? interval : prev + (gap - prev) / 8;
		lastSent = now;
		Deliver(bgrx.data(), srcWidth, srcHeight, scratch.data(), format, now - begin, first);
	}
	_actualInterval = 0;

	shared.Close();
	Log(L"DirectShow: stream stopped");
	if (SUCCEEDED(com))
		CoUninitialize();
}

void Pin::Deliver(const uint8_t* bgrx, int srcWidth, int srcHeight, uint8_t* scratch, StreamFormat& format, REFERENCE_TIME fallbackTime, bool& first)
{
	IMediaSample* sample = nullptr;
	if (FAILED(_allocator->GetBuffer(&sample, nullptr, nullptr, 0)) || !sample)
		return; // decommitted: we're stopping

	// a renderer can ask for a new row pitch through the sample's media type
	AM_MEDIA_TYPE* changed = nullptr;
	if (sample->GetMediaType(&changed) == S_OK && changed)
	{
		StreamFormat next;
		if (DescribeFormat(*changed, next) && next.pixel == format.pixel && next.width == format.width && next.height == format.height)
		{
			next.fps = format.fps;
			format = next;
		}
		DeleteMediaType(changed);
	}

	BYTE* data = nullptr;
	if (FAILED(sample->GetPointer(&data)) || !data || sample->GetSize() < (long)format.size)
	{
		sample->Release();
		return;
	}

	// the server runs 640x480 or 320x240; scale to what this app asked for
	const int w = format.width, h = format.height;
	const uint8_t* src = bgrx;
	long srcStride = (long)srcWidth * 4;
	if (srcWidth == w * 2 && srcHeight == h * 2)
		ps3eye::HalveBGRX(bgrx, srcWidth, srcHeight, srcStride, scratch, (long)w * 4);
	else if (srcWidth * 2 == w && srcHeight * 2 == h)
		ps3eye::DoubleBGRX(bgrx, srcWidth, srcHeight, srcStride, scratch, (long)w * 4);
	else if (srcWidth != w || srcHeight != h)
	{
		sample->Release();
		return;
	}
	if (srcWidth != w)
	{
		src = scratch;
		srcStride = (long)w * 4;
	}

	BYTE* top = format.bottomUp ? data + (size_t)(h - 1) * format.stride : data;
	const long stride = format.bottomUp ? -format.stride : format.stride;
	switch (format.pixel)
	{
	case Pixel::YUY2: ps3eye::BGRXToYUY2(src, w, h, srcStride, data, format.stride); break;
	case Pixel::NV12: ps3eye::BGRXToNV12(src, w, h, srcStride, data, format.stride); break;
	case Pixel::I420: ps3eye::BGRXToI420(src, w, h, srcStride, data, format.stride); break;
	case Pixel::RGB24: ps3eye::BGRXToBGR(src, w, h, srcStride, top, stride); break;
	case Pixel::RGB32: ps3eye::CopyBGRX(src, w, h, srcStride, top, stride); break;
	}

	REFERENCE_TIME start;
	if (!_filter->StreamTime(start))
		start = fallbackTime;
	REFERENCE_TIME stop = start + 10000000LL / std::max<uint32_t>(1, format.fps);
	sample->SetTime(&start, &stop);
	sample->SetMediaTime(nullptr, nullptr);
	sample->SetSyncPoint(TRUE);
	sample->SetPreroll(FALSE);
	sample->SetDiscontinuity(first ? TRUE : FALSE);
	sample->SetActualDataLength((long)format.size);
	first = false;

	_input->Receive(sample);
	sample->Release();
}

// ==== Filter ===============================================================================

Filter::Filter() : _pin(this)
{
	InitializeCriticalSection(&_lock);
	gObjects++;
}

Filter::~Filter()
{
	Stop();
	if (_clock)
		_clock->Release();
	// _pin is destroyed after this body; its destructor doesn't take the lock
	DeleteCriticalSection(&_lock);
	gObjects--;
}

STDMETHODIMP Filter::QueryInterface(REFIID riid, void** ppv)
{
	if (!ppv)
		return E_POINTER;
	if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter || riid == IID_IBaseFilter)
		*ppv = static_cast<IBaseFilter*>(this);
	else if (riid == IID_IAMFilterMiscFlags)
		*ppv = static_cast<IAMFilterMiscFlags*>(this);
	else if (riid == IID_IAMVideoControl)
		*ppv = static_cast<IAMVideoControl*>(this);
	else if (riid == IID_ISpecifyPropertyPages)
		*ppv = static_cast<ISpecifyPropertyPages*>(this);
	else if (riid == IID_IAMVideoProcAmp)
		*ppv = static_cast<IAMVideoProcAmp*>(&_procAmp);
	else if (riid == IID_IAMCameraControl)
		*ppv = static_cast<IAMCameraControl*>(&_cameraControl);
	else
	{
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	AddRef();
	return S_OK;
}

STDMETHODIMP_(ULONG) Filter::AddRef() { return (ULONG)++_refs; }

STDMETHODIMP_(ULONG) Filter::Release()
{
	long n = --_refs;
	if (!n)
		delete this;
	return (ULONG)n;
}

STDMETHODIMP Filter::GetClassID(CLSID* clsid)
{
	if (!clsid)
		return E_POINTER;
	*clsid = CLSID_PS3EyeFilter;
	return S_OK;
}

STDMETHODIMP Filter::Stop()
{
	bool wasActive;
	{
		Lock lock(_lock);
		wasActive = _state != State_Stopped;
		_state = State_Stopped;
		_running = false;
	}
	if (wasActive)
		_pin.Inactive(); // joins the streaming thread: not under the lock
	return S_OK;
}

STDMETHODIMP Filter::Pause()
{
	Lock lock(_lock);
	if (_state == State_Stopped)
	{
		HRESULT hr = _pin.Active();
		if (FAILED(hr))
			return hr;
	}
	_state = State_Paused;
	_running = false;
	return S_OK;
}

STDMETHODIMP Filter::Run(REFERENCE_TIME start)
{
	Lock lock(_lock);
	if (_state == State_Stopped)
	{
		HRESULT hr = _pin.Active();
		if (FAILED(hr))
			return hr;
	}
	_start = start;
	_state = State_Running;
	_running = true;
	return S_OK;
}

STDMETHODIMP Filter::GetState(DWORD, FILTER_STATE* state)
{
	if (!state)
		return E_POINTER;
	Lock lock(_lock);
	*state = _state;
	return _state == State_Paused ? VFW_S_CANT_CUE : S_OK; // live source: nothing while paused
}

STDMETHODIMP Filter::SetSyncSource(IReferenceClock* clock)
{
	Lock lock(_lock);
	if (clock)
		clock->AddRef();
	if (_clock)
		_clock->Release();
	_clock = clock;
	return S_OK;
}

STDMETHODIMP Filter::GetSyncSource(IReferenceClock** clock)
{
	if (!clock)
		return E_POINTER;
	Lock lock(_lock);
	*clock = _clock;
	if (_clock)
		_clock->AddRef();
	return S_OK;
}

bool Filter::StreamTime(REFERENCE_TIME& t)
{
	Lock lock(_lock);
	if (!_clock || _state != State_Running)
		return false;
	REFERENCE_TIME now = 0;
	if (FAILED(_clock->GetTime(&now)))
		return false;
	t = now - _start;
	return true;
}

STDMETHODIMP Filter::EnumPins(IEnumPins** ppEnum)
{
	if (!ppEnum)
		return E_POINTER;
	*ppEnum = new (std::nothrow) PinEnum(this, &_pin, 0);
	return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP Filter::FindPin(LPCWSTR id, IPin** pin)
{
	if (!id || !pin)
		return E_POINTER;
	if (wcscmp(id, kPinName) != 0)
	{
		*pin = nullptr;
		return VFW_E_NOT_FOUND;
	}
	*pin = &_pin;
	_pin.AddRef();
	return S_OK;
}

STDMETHODIMP Filter::QueryFilterInfo(FILTER_INFO* info)
{
	if (!info)
		return E_POINTER;
	Lock lock(_lock);
	wcscpy_s(info->achName, _name);
	info->pGraph = _graph;
	if (_graph)
		_graph->AddRef();
	return S_OK;
}

STDMETHODIMP Filter::JoinFilterGraph(IFilterGraph* graph, LPCWSTR name)
{
	Lock lock(_lock);
	_graph = graph;
	wcsncpy_s(_name, name ? name : L"", _TRUNCATE);
	return S_OK;
}

STDMETHODIMP Filter::QueryVendorInfo(LPWSTR*)
{
	return E_NOTIMPL;
}

STDMETHODIMP_(ULONG) Filter::GetMiscFlags()
{
	return AM_FILTER_MISC_FLAGS_IS_SOURCE;
}

// ---- IAMVideoControl ----

STDMETHODIMP Filter::GetCaps(IPin*, long* flags)
{
	if (!flags)
		return E_POINTER;
	*flags = VideoControlFlag_FlipHorizontal | VideoControlFlag_FlipVertical;
	return S_OK;
}

STDMETHODIMP Filter::SetMode(IPin*, long mode)
{
	if (mode & ~(VideoControlFlag_FlipHorizontal | VideoControlFlag_FlipVertical))
		return E_INVALIDARG;
	return SetFlipMode(mode);
}

STDMETHODIMP Filter::GetMode(IPin*, long* mode)
{
	if (!mode)
		return E_POINTER;
	*mode = GetFlipMode();
	return S_OK;
}

STDMETHODIMP Filter::GetCurrentActualFrameRate(IPin*, LONGLONG* interval)
{
	if (!interval)
		return E_POINTER;
	*interval = _pin._actualInterval;
	return *interval ? S_OK : VFW_E_NOT_CONNECTED;
}

STDMETHODIMP Filter::GetMaxAvailableFrameRate(IPin*, long, SIZE dimensions, LONGLONG* interval)
{
	if (!interval)
		return E_POINTER;
	if (!(dimensions.cx == 640 && dimensions.cy == 480) && !(dimensions.cx == 320 && dimensions.cy == 240))
		return E_INVALIDARG;
	*interval = 10000000LL / MaxFps((uint32_t)dimensions.cx);
	return S_OK;
}

STDMETHODIMP Filter::GetFrameRateList(IPin*, long, SIZE dimensions, long* count, LONGLONG** intervals)
{
	if (!count)
		return E_POINTER;
	if (!(dimensions.cx == 640 && dimensions.cy == 480) && !(dimensions.cx == 320 && dimensions.cy == 240))
		return E_INVALIDARG;
	size_t n = 0;
	const uint16_t* rates = ps3eye::SupportedRates((uint32_t)dimensions.cx, &n);
	*count = (long)n;
	if (!intervals)
		return S_OK; // just the count
	*intervals = (LONGLONG*)CoTaskMemAlloc(n * sizeof(LONGLONG));
	if (!*intervals)
		return E_OUTOFMEMORY;
	for (size_t i = 0; i < n; i++)
		(*intervals)[i] = 10000000LL / rates[i];
	return S_OK;
}

// ---- ISpecifyPropertyPages ----

STDMETHODIMP Filter::GetPages(CAUUID* pages)
{
	if (!pages)
		return E_POINTER;
	pages->pElems = (GUID*)CoTaskMemAlloc(2 * sizeof(GUID));
	if (!pages->pElems)
	{
		pages->cElems = 0;
		return E_OUTOFMEMORY;
	}
	pages->cElems = 2;
	pages->pElems[0] = CLSID_VideoProcAmpPropertyPage;
	pages->pElems[1] = CLSID_CameraControlPropertyPage;
	return S_OK;
}

// ==== image controls =======================================================================

namespace {

HRESULT ControlRangeOf(ControlGroup group, long prop, long* min, long* max, long* step, long* def, long* caps)
{
	if (!min || !max || !step || !def || !caps)
		return E_POINTER;
	ControlRange r;
	if (!GetControlRange(group, prop, r))
		return E_PROP_ID_UNSUPPORTED;
	*min = r.min;
	*max = r.max;
	*step = r.step;
	*def = r.def;
	// VideoProcAmp_Flags_* and CameraControl_Flags_* share values: 1 = auto, 2 = manual
	*caps = VideoProcAmp_Flags_Manual | (r.canAuto ? VideoProcAmp_Flags_Auto : 0);
	return S_OK;
}

HRESULT ControlGetOf(ControlGroup group, long prop, long* value, long* flags)
{
	if (!value || !flags)
		return E_POINTER;
	bool autoMode = false;
	if (!GetControl(group, prop, *value, autoMode))
		return E_PROP_ID_UNSUPPORTED;
	*flags = autoMode ? VideoProcAmp_Flags_Auto : VideoProcAmp_Flags_Manual;
	return S_OK;
}

HRESULT ControlSetOf(ControlGroup group, long prop, long value, long flags)
{
	ControlRange r;
	if (!GetControlRange(group, prop, r))
		return E_PROP_ID_UNSUPPORTED;
	const bool autoMode = (flags & VideoProcAmp_Flags_Auto) != 0;
	if (autoMode && !r.canAuto)
		return E_INVALIDARG;
	if (!autoMode && (value < r.min || value > r.max))
		return E_INVALIDARG;
	HRESULT hr = SetControl(group, prop, value, autoMode);
	if (FAILED(hr))
		Log(L"DirectShow: can't save image setting %ld (0x%08lx); run Install again to allow it", prop, hr);
	return hr;
}

} // namespace

STDMETHODIMP ProcAmp::QueryInterface(REFIID riid, void** ppv) { return _filter->QueryInterface(riid, ppv); }
STDMETHODIMP_(ULONG) ProcAmp::AddRef() { return _filter->AddRef(); }
STDMETHODIMP_(ULONG) ProcAmp::Release() { return _filter->Release(); }

STDMETHODIMP ProcAmp::GetRange(long prop, long* min, long* max, long* step, long* def, long* caps)
{
	return ControlRangeOf(ControlGroup::ProcAmp, prop, min, max, step, def, caps);
}

STDMETHODIMP ProcAmp::Set(long prop, long value, long flags)
{
	return ControlSetOf(ControlGroup::ProcAmp, prop, value, flags);
}

STDMETHODIMP ProcAmp::Get(long prop, long* value, long* flags)
{
	return ControlGetOf(ControlGroup::ProcAmp, prop, value, flags);
}

STDMETHODIMP CameraControl::QueryInterface(REFIID riid, void** ppv) { return _filter->QueryInterface(riid, ppv); }
STDMETHODIMP_(ULONG) CameraControl::AddRef() { return _filter->AddRef(); }
STDMETHODIMP_(ULONG) CameraControl::Release() { return _filter->Release(); }

STDMETHODIMP CameraControl::GetRange(long prop, long* min, long* max, long* step, long* def, long* caps)
{
	return ControlRangeOf(ControlGroup::Camera, prop, min, max, step, def, caps);
}

STDMETHODIMP CameraControl::Set(long prop, long value, long flags)
{
	return ControlSetOf(ControlGroup::Camera, prop, value, flags);
}

STDMETHODIMP CameraControl::Get(long prop, long* value, long* flags)
{
	return ControlGetOf(ControlGroup::Camera, prop, value, flags);
}

} // namespace vcam
