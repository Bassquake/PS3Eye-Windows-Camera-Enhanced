#pragma once

#include <atomic>
#include <deque>
#include <thread>
#include <vector>

// One video stream fed by the PS3 Eye. Media Foundation pulls samples with RequestSample();
// a capture thread owns the camera and answers pending requests as frames arrive.
struct MediaStream : winrt::implements<MediaStream, CBaseAttributes<IMFAttributes>, IMFMediaStream2, IKsControl>
{
public:
	// IMFMediaEventGenerator
	STDMETHOD(BeginGetEvent)(IMFAsyncCallback* pCallback, IUnknown* punkState);
	STDMETHOD(EndGetEvent)(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent);
	STDMETHOD(GetEvent)(DWORD dwFlags, IMFMediaEvent** ppEvent);
	STDMETHOD(QueueEvent)(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue);

	// IMFMediaStream
	STDMETHOD(GetMediaSource)(IMFMediaSource** ppMediaSource);
	STDMETHOD(GetStreamDescriptor)(IMFStreamDescriptor** ppStreamDescriptor);
	STDMETHOD(RequestSample)(IUnknown* pToken);

	// IMFMediaStream2
	STDMETHOD(SetStreamState)(MF_STREAM_STATE value);
	STDMETHOD(GetStreamState)(MF_STREAM_STATE* value);

	// IKsControl
	STDMETHOD_(NTSTATUS, KsProperty)(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData, ULONG DataLength, ULONG* BytesReturned);
	STDMETHOD_(NTSTATUS, KsMethod)(PKSMETHOD Method, ULONG MethodLength, LPVOID MethodData, ULONG DataLength, ULONG* BytesReturned);
	STDMETHOD_(NTSTATUS, KsEvent)(PKSEVENT Event, ULONG EventLength, LPVOID EventData, ULONG DataLength, ULONG* BytesReturned);

public:
	MediaStream() :
		_index(0),
		_state(MF_STREAM_STATE_STOPPED),
		_format(GUID_NULL),
		_width(640),
		_height(480),
		_fps(30)
	{
		SetBaseAttributesTraceName(L"MediaStreamAtts");
	}

	~MediaStream()
	{
		StopCapture();
	}

	HRESULT Initialize(IMFMediaSource* source, int index);
	HRESULT SetAllocator(IUnknown* allocator);
	MFSampleAllocatorUsage GetAllocatorUsage();
	HRESULT SetD3DManager(IUnknown* manager);
	HRESULT Start(IMFMediaType* type);
	HRESULT Stop();
	void Shutdown();

private:
#if _DEBUG
	int32_t query_interface_tearoff(winrt::guid const& id, void** object) const noexcept override
	{
		RETURN_HR_MSG(E_NOINTERFACE, "MediaStream QueryInterface failed on IID %s", GUID_ToStringW(id).c_str());
	}
#endif

	void StartCapture();
	void StopCapture();
	void CaptureLoop();
	void Deliver(const uint8_t* bgrx, MFTIME time);
	HRESULT FillSample(IMFSample* sample, const uint8_t* bgrx);

	winrt::slim_mutex _lock;
	MF_STREAM_STATE _state;
	GUID _format;
	UINT32 _width;
	UINT32 _height;
	UINT32 _fps;
	wil::com_ptr_nothrow<IMFMediaType> _currentType;
	wil::com_ptr_nothrow<IMFStreamDescriptor> _descriptor;
	wil::com_ptr_nothrow<IMFMediaEventQueue> _queue;
	wil::com_ptr_nothrow<IMFMediaSource> _source;
	wil::com_ptr_nothrow<IMFVideoSampleAllocatorEx> _allocator;
	std::deque<wil::com_ptr_nothrow<IUnknown>> _requests; // one entry per RequestSample (token may be null)
	int _index;

	std::thread _thread;
	std::atomic<bool> _quit{ false };
};
