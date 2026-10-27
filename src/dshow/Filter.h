// "PS3 Eye Camera" as a DirectShow video capture source (Windows 10 path). Registered in the
// video input device category, so Zoom, Teams, Discord, OBS and browsers list it like a webcam.
// One output pin; its streaming thread reads frames that PS3EyeServer.exe shares (SharedFrames.h)
// and pushes them downstream in the negotiated format. Image controls (IAMVideoProcAmp,
// IAMCameraControl, IAMVideoControl flips, the standard property pages) go through Controls.h.
//
// Written against the raw DirectShow interfaces (no base classes). Locking: the filter's lock
// guards state and the pin's connection; the streaming thread never takes it while delivering,
// and Stop() joins the thread without holding it.
#pragma once

#include <windows.h>
#include <dshow.h>

#include <atomic>
#include <thread>

#include "MediaTypes.h"

namespace vcam {

extern const CLSID CLSID_PS3EyeFilter;
extern HMODULE gModule;
extern std::atomic<long> gObjects;   // live COM objects + LockServer count, for DllCanUnloadNow

class Filter;

class Pin final : public IPin, public IAMStreamConfig, public IKsPropertySet
{
public:
	explicit Pin(Filter* filter);
	~Pin();

	// IUnknown (lifetime is the filter's)
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
	STDMETHODIMP_(ULONG) AddRef() override;
	STDMETHODIMP_(ULONG) Release() override;

	// IPin
	STDMETHODIMP Connect(IPin* receivePin, const AM_MEDIA_TYPE* pmt) override;
	STDMETHODIMP ReceiveConnection(IPin* connector, const AM_MEDIA_TYPE* pmt) override;
	STDMETHODIMP Disconnect() override;
	STDMETHODIMP ConnectedTo(IPin** pin) override;
	STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
	STDMETHODIMP QueryPinInfo(PIN_INFO* info) override;
	STDMETHODIMP QueryDirection(PIN_DIRECTION* dir) override;
	STDMETHODIMP QueryId(LPWSTR* id) override;
	STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
	STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
	STDMETHODIMP QueryInternalConnections(IPin** pins, ULONG* count) override;
	STDMETHODIMP EndOfStream() override;
	STDMETHODIMP BeginFlush() override;
	STDMETHODIMP EndFlush() override;
	STDMETHODIMP NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) override;

	// IAMStreamConfig
	STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
	STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
	STDMETHODIMP GetNumberOfCapabilities(int* count, int* size) override;
	STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** ppmt, BYTE* scc) override;

	// IKsPropertySet (pin category = capture)
	STDMETHODIMP Set(REFGUID set, DWORD id, LPVOID instance, DWORD instanceSize, LPVOID data, DWORD dataSize) override;
	STDMETHODIMP Get(REFGUID set, DWORD id, LPVOID instance, DWORD instanceSize, LPVOID data, DWORD dataSize, DWORD* returned) override;
	STDMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD* support) override;

	// called by the filter (under its lock) on Stopped <-> Paused
	HRESULT Active();
	void Inactive();   // must be called WITHOUT the filter lock

private:
	HRESULT TryConnect(IPin* receivePin, const AM_MEDIA_TYPE& mt);
	HRESULT DecideAllocator(IMemInputPin* input, const StreamFormat& format);
	void ReleaseConnection();
	void Stream(StreamFormat format);
	void Deliver(const uint8_t* bgrx, int srcWidth, int srcHeight, uint8_t* scratch, StreamFormat& format, REFERENCE_TIME fallbackTime, bool& first);

	Filter* _filter;
	IPin* _connected = nullptr;
	IMemInputPin* _input = nullptr;
	IMemAllocator* _allocator = nullptr;
	AM_MEDIA_TYPE _mt{};            // valid while connected
	AM_MEDIA_TYPE* _preferred = nullptr; // from IAMStreamConfig::SetFormat

	std::thread _thread;
	std::atomic<bool> _quit{ false };
	std::atomic<LONGLONG> _actualInterval{ 0 };   // measured time between delivered frames (100 ns)
	friend class Filter;
};

// IAMVideoProcAmp and IAMCameraControl have identically shaped methods, so each lives in its own
// object; both share the filter's lifetime like the pin does.
class ProcAmp final : public IAMVideoProcAmp
{
public:
	explicit ProcAmp(Filter* filter) : _filter(filter) {}
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
	STDMETHODIMP_(ULONG) AddRef() override;
	STDMETHODIMP_(ULONG) Release() override;
	STDMETHODIMP GetRange(long prop, long* min, long* max, long* step, long* def, long* caps) override;
	STDMETHODIMP Set(long prop, long value, long flags) override;
	STDMETHODIMP Get(long prop, long* value, long* flags) override;

private:
	Filter* _filter;
};

class CameraControl final : public IAMCameraControl
{
public:
	explicit CameraControl(Filter* filter) : _filter(filter) {}
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
	STDMETHODIMP_(ULONG) AddRef() override;
	STDMETHODIMP_(ULONG) Release() override;
	STDMETHODIMP GetRange(long prop, long* min, long* max, long* step, long* def, long* caps) override;
	STDMETHODIMP Set(long prop, long value, long flags) override;
	STDMETHODIMP Get(long prop, long* value, long* flags) override;

private:
	Filter* _filter;
};

class Filter final : public IBaseFilter, public IAMFilterMiscFlags, public IAMVideoControl, public ISpecifyPropertyPages
{
public:
	Filter();
	~Filter();

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
	STDMETHODIMP_(ULONG) AddRef() override;
	STDMETHODIMP_(ULONG) Release() override;

	// IPersist
	STDMETHODIMP GetClassID(CLSID* clsid) override;

	// IMediaFilter
	STDMETHODIMP Stop() override;
	STDMETHODIMP Pause() override;
	STDMETHODIMP Run(REFERENCE_TIME start) override;
	STDMETHODIMP GetState(DWORD timeout, FILTER_STATE* state) override;
	STDMETHODIMP SetSyncSource(IReferenceClock* clock) override;
	STDMETHODIMP GetSyncSource(IReferenceClock** clock) override;

	// IBaseFilter
	STDMETHODIMP EnumPins(IEnumPins** ppEnum) override;
	STDMETHODIMP FindPin(LPCWSTR id, IPin** pin) override;
	STDMETHODIMP QueryFilterInfo(FILTER_INFO* info) override;
	STDMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override;
	STDMETHODIMP QueryVendorInfo(LPWSTR* info) override;

	// IAMFilterMiscFlags
	STDMETHODIMP_(ULONG) GetMiscFlags() override;

	// IAMVideoControl (mirror/flip, frame-rate lists)
	STDMETHODIMP GetCaps(IPin* pin, long* flags) override;
	STDMETHODIMP SetMode(IPin* pin, long mode) override;
	STDMETHODIMP GetMode(IPin* pin, long* mode) override;
	STDMETHODIMP GetCurrentActualFrameRate(IPin* pin, LONGLONG* interval) override;
	STDMETHODIMP GetMaxAvailableFrameRate(IPin* pin, long index, SIZE dimensions, LONGLONG* interval) override;
	STDMETHODIMP GetFrameRateList(IPin* pin, long index, SIZE dimensions, long* count, LONGLONG** intervals) override;

	// ISpecifyPropertyPages (Windows' own Video Proc Amp and Camera Control pages)
	STDMETHODIMP GetPages(CAUUID* pages) override;

private:
	friend class Pin;
	friend class ProcAmp;
	friend class CameraControl;

	// stream time now, or false when there's no clock / not running
	bool StreamTime(REFERENCE_TIME& t);

	std::atomic<long> _refs{ 1 };
	CRITICAL_SECTION _lock;
	FILTER_STATE _state = State_Stopped;
	std::atomic<bool> _running{ false };
	IReferenceClock* _clock = nullptr;
	REFERENCE_TIME _start = 0;
	IFilterGraph* _graph = nullptr;   // not AddRef'ed: the graph owns us
	wchar_t _name[MAX_FILTER_NAME] = L"";
	Pin _pin;
	ProcAmp _procAmp{ this };
	CameraControl _cameraControl{ this };
};

} // namespace vcam
