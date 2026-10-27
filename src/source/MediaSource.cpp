#include "pch.h"
#include "Undocumented.h"
#include "Tools.h"
#include "EnumNames.h"
#include "MFTools.h"
#include "MediaStream.h"
#include "MediaSource.h"
#include "Config.h"
#include "Controls.h"

namespace {

// ---- image controls as KS properties (what apps' IAMVideoProcAmp / IAMCameraControl and the
// Windows camera settings page turn into on Windows 11) ----

constexpr HRESULT kMoreData = HRESULT_FROM_WIN32(ERROR_MORE_DATA);
constexpr HRESULT kNotFound = HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);

template <typename T> HRESULT Reply(const T& value, LPVOID data, ULONG dataLength, ULONG* returned)
{
	*returned = sizeof(T);
	if (!data || dataLength < sizeof(T))
		return kMoreData;
	memcpy(data, &value, sizeof(T));
	return S_OK;
}

// Range (BASICSUPPORT) and default (DEFAULTVALUES) replies for one LONG property.
HRESULT DescribeRange(const KSPROPERTY* p, const vcam::ControlRange& r, LPVOID data, ULONG dataLength, ULONG* returned)
{
	KSPROPERTY_DESCRIPTION desc{};
	desc.AccessFlags = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_DEFAULTVALUES;
	desc.PropTypeSet.Set = KSPROPTYPESETID_General;
	desc.PropTypeSet.Id = VT_I4;
	desc.MembersListCount = 1;

	if (p->Flags & KSPROPERTY_TYPE_BASICSUPPORT)
	{
		if (data && dataLength == sizeof(ULONG))
			return Reply<ULONG>(desc.AccessFlags, data, dataLength, returned);
		struct
		{
			KSPROPERTY_DESCRIPTION desc;
			KSPROPERTY_MEMBERSHEADER header;
			KSPROPERTY_STEPPING_LONG stepping;
		} full{};
		full.desc = desc;
		full.desc.DescriptionSize = sizeof(full);
		full.header.MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
		full.header.MembersSize = sizeof(KSPROPERTY_STEPPING_LONG);
		full.header.MembersCount = 1;
		full.stepping.SteppingDelta = (ULONG)r.step;
		full.stepping.Bounds.SignedMinimum = r.min;
		full.stepping.Bounds.SignedMaximum = r.max;
		if (data && dataLength >= sizeof(KSPROPERTY_DESCRIPTION) && dataLength < sizeof(full))
			return Reply(full.desc, data, dataLength, returned); // just the header, to learn the size
		return Reply(full, data, dataLength, returned);
	}

	struct
	{
		KSPROPERTY_DESCRIPTION desc;
		KSPROPERTY_MEMBERSHEADER header;
		LONG value;
	} def{};
	def.desc = desc;
	def.desc.DescriptionSize = sizeof(def);
	def.header.MembersFlags = KSPROPERTY_MEMBER_VALUES;
	def.header.MembersSize = sizeof(LONG);
	def.header.MembersCount = 1;
	def.header.Flags = KSPROPERTY_MEMBER_FLAG_DEFAULT;
	def.value = r.def;
	return Reply(def, data, dataLength, returned);
}

// KSPROPERTY_VIDEOPROCAMP_S and KSPROPERTY_CAMERACONTROL_S have the same layout, and their
// flag values agree (1 = auto, 2 = manual).
HRESULT ControlProperty(vcam::ControlGroup group, const KSPROPERTY* p, ULONG length, LPVOID data, ULONG dataLength, ULONG* returned)
{
	static_assert(sizeof(KSPROPERTY_VIDEOPROCAMP_S) == sizeof(KSPROPERTY_CAMERACONTROL_S));
	vcam::ControlRange r;
	if (!vcam::GetControlRange(group, (long)p->Id, r))
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	if (p->Flags & (KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_DEFAULTVALUES))
		return DescribeRange(p, r, data, dataLength, returned);

	if (p->Flags & KSPROPERTY_TYPE_GET)
	{
		KSPROPERTY_VIDEOPROCAMP_S out{};
		out.Property = *p;
		long value = 0;
		bool autoMode = false;
		vcam::GetControl(group, (long)p->Id, value, autoMode);
		out.Value = value;
		out.Flags = autoMode ? KSPROPERTY_VIDEOPROCAMP_FLAGS_AUTO : KSPROPERTY_VIDEOPROCAMP_FLAGS_MANUAL;
		out.Capabilities = KSPROPERTY_VIDEOPROCAMP_FLAGS_MANUAL | (r.canAuto ? KSPROPERTY_VIDEOPROCAMP_FLAGS_AUTO : 0);
		return Reply(out, data, dataLength, returned);
	}

	if (p->Flags & KSPROPERTY_TYPE_SET)
	{
		// the new value comes in the data buffer (some callers put it in the property buffer)
		const KSPROPERTY_VIDEOPROCAMP_S* in = nullptr;
		if (data && dataLength >= sizeof(KSPROPERTY_VIDEOPROCAMP_S))
			in = (const KSPROPERTY_VIDEOPROCAMP_S*)data;
		else if (length >= sizeof(KSPROPERTY_VIDEOPROCAMP_S))
			in = (const KSPROPERTY_VIDEOPROCAMP_S*)p;
		if (!in)
			return E_INVALIDARG;
		*returned = 0;
		const bool autoMode = (in->Flags & KSPROPERTY_VIDEOPROCAMP_FLAGS_AUTO) != 0;
		if (autoMode && !r.canAuto)
			return E_INVALIDARG;
		HRESULT hr = vcam::SetControl(group, (long)p->Id, in->Value, autoMode);
		if (FAILED(hr))
			vcam::Log(L"Can't save image setting %lu (0x%08lx); run Install again to allow it", p->Id, hr);
		return hr;
	}
	return E_INVALIDARG;
}

// Mirror/flip: KSPROPERTY_VIDEOCONTROL_CAPS and _MODE.
HRESULT VideoControlProperty(const KSPROPERTY* p, ULONG length, LPVOID data, ULONG dataLength, ULONG* returned)
{
	constexpr LONG kFlips = KS_VideoControlFlag_FlipHorizontal | KS_VideoControlFlag_FlipVertical;
	if (p->Id == KSPROPERTY_VIDEOCONTROL_CAPS && (p->Flags & KSPROPERTY_TYPE_GET))
	{
		KSPROPERTY_VIDEOCONTROL_CAPS_S out{};
		out.Property = *p;
		if (length >= sizeof(out))
			out.StreamIndex = ((const KSPROPERTY_VIDEOCONTROL_CAPS_S*)p)->StreamIndex;
		out.VideoControlCaps = kFlips;
		return Reply(out, data, dataLength, returned);
	}
	if (p->Id == KSPROPERTY_VIDEOCONTROL_MODE)
	{
		if (p->Flags & KSPROPERTY_TYPE_GET)
		{
			KSPROPERTY_VIDEOCONTROL_MODE_S out{};
			out.Property = *p;
			if (length >= sizeof(out))
				out.StreamIndex = ((const KSPROPERTY_VIDEOCONTROL_MODE_S*)p)->StreamIndex;
			out.Mode = vcam::GetFlipMode();
			return Reply(out, data, dataLength, returned);
		}
		if (p->Flags & KSPROPERTY_TYPE_SET)
		{
			const KSPROPERTY_VIDEOCONTROL_MODE_S* in = nullptr;
			if (data && dataLength >= sizeof(KSPROPERTY_VIDEOCONTROL_MODE_S))
				in = (const KSPROPERTY_VIDEOCONTROL_MODE_S*)data;
			else if (length >= sizeof(KSPROPERTY_VIDEOCONTROL_MODE_S))
				in = (const KSPROPERTY_VIDEOCONTROL_MODE_S*)p;
			if (!in || (in->Mode & ~kFlips))
				return E_INVALIDARG;
			*returned = 0;
			return vcam::SetFlipMode(in->Mode);
		}
	}
	return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

} // namespace

HRESULT MediaSource::Initialize(IMFAttributes* attributes)
{
	if (attributes)
	{
		RETURN_IF_FAILED(attributes->CopyAllItems(this));
	}

	wil::com_ptr_nothrow<IMFSensorProfileCollection> collection;
	RETURN_IF_FAILED(MFCreateSensorProfileCollection(&collection));

	DWORD streamId = 0;
	wil::com_ptr_nothrow<IMFSensorProfile> profile;
	RETURN_IF_FAILED(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &profile));
	RETURN_IF_FAILED(profile->AddProfileFilter(streamId, L"((RES==;FRT<=30,1;SUT==))"));
	RETURN_IF_FAILED(collection->AddProfile(profile.get()));

	RETURN_IF_FAILED(MFCreateSensorProfile(KSCAMERAPROFILE_HighFrameRate, 0, nullptr, &profile));
	RETURN_IF_FAILED(profile->AddProfileFilter(streamId, L"((RES==;FRT>=60,1;SUT==))"));
	RETURN_IF_FAILED(collection->AddProfile(profile.get()));
	RETURN_IF_FAILED(SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, collection.get()));

	try
	{
		auto appInfo = winrt::Windows::ApplicationModel::AppInfo::Current();
		if (appInfo)
		{
			RETURN_IF_FAILED(SetString(MF_VIRTUALCAMERA_CONFIGURATION_APP_PACKAGE_FAMILY_NAME, appInfo.PackageFamilyName().data()));
		}
	}
	catch (...)
	{
		WINTRACE(L"MediaSource::Initialize no AppX");
	}

	auto streams = wil::make_unique_cotaskmem_array<wil::com_ptr_nothrow<IMFStreamDescriptor>>(_streams.size());
	for (uint32_t i = 0; i < streams.size(); i++)
	{
		wil::com_ptr_nothrow<IMFStreamDescriptor> desc;
		RETURN_IF_FAILED(_streams[i]->GetStreamDescriptor(&desc));
		streams[i] = desc.detach();
	}
	RETURN_IF_FAILED(MFCreatePresentationDescriptor((DWORD)streams.size(), streams.get(), &_descriptor));
	RETURN_IF_FAILED(MFCreateEventQueue(&_queue));
	return S_OK;
}

int MediaSource::GetStreamIndexById(DWORD id)
{
	for (uint32_t i = 0; i < _streams.size(); i++)
	{
		wil::com_ptr_nothrow<IMFStreamDescriptor> desc;
		if (FAILED(_streams[i]->GetStreamDescriptor(&desc)))
			return -1;

		DWORD sid = 0;
		if (FAILED(desc->GetStreamIdentifier(&sid)))
			return -1;

		if (sid == id)
			return i;
	}
	return -1;
}

// IMFMediaEventGenerator
STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
	WINTRACE(L"MediaSource::BeginGetEvent pCallback:%p punkState:%p", pCallback, punkState);
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->BeginGetEvent(pCallback, punkState));
	return S_OK;
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
	WINTRACE(L"MediaSource::EndGetEvent");
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->EndGetEvent(pResult, ppEvent));
	return S_OK;
}

STDMETHODIMP MediaSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
	WINTRACE(L"MediaSource::GetEvent");
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->GetEvent(dwFlags, ppEvent));
	return S_OK;
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
	WINTRACE(L"MediaSource::QueueEvent");
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue));
	return S_OK;
}

// IMFMediaSource
STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppPresentationDescriptor)
{
	WINTRACE(L"MediaSource::CreatePresentationDescriptor");
	RETURN_HR_IF_NULL(E_POINTER, ppPresentationDescriptor);
	*ppPresentationDescriptor = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_descriptor);
	
	RETURN_IF_FAILED(_descriptor->Clone(ppPresentationDescriptor));
	return S_OK;
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* pdwCharacteristics)
{
	WINTRACE(L"MediaSource::GetCharacteristics");
	RETURN_HR_IF_NULL(E_POINTER, pdwCharacteristics);

	*pdwCharacteristics = MFMEDIASOURCE_IS_LIVE;
	return S_OK;
}

STDMETHODIMP MediaSource::Pause()
{
	WINTRACE(L"MediaSource::Pause");
	RETURN_HR(MF_E_INVALID_STATE_TRANSITION);
}

STDMETHODIMP MediaSource::Shutdown()
{
	WINTRACE(L"MediaSource::Shutdown");
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	LOG_IF_FAILED_MSG(_queue->Shutdown(), "Queue shutdown failed");
	_queue.reset();

	for (uint32_t i = 0; i < _streams.size(); i++)
	{
		_streams[i]->Shutdown();
	}

	_descriptor.reset();
	_attributes.reset();
	return S_OK;
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition)
{
	WINTRACE(L"MediaSource::Start pPresentationDescriptor:%p pguidTimeFormat:%p pvarStartPosition:%p", pPresentationDescriptor, pguidTimeFormat, pvarStartPosition);
	RETURN_HR_IF_NULL(E_POINTER, pPresentationDescriptor);
	RETURN_HR_IF_NULL(E_POINTER, pvarStartPosition);
	RETURN_HR_IF_MSG(E_INVALIDARG, pguidTimeFormat && *pguidTimeFormat != GUID_NULL, "Unsupported guid time format");
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_descriptor);

	DWORD count;
	RETURN_IF_FAILED(pPresentationDescriptor->GetStreamDescriptorCount(&count));
	RETURN_HR_IF_MSG(E_INVALIDARG, count != (DWORD)_streams.size(), "Invalid number of descriptor streams");

	wil::unique_prop_variant time;
	RETURN_IF_FAILED(InitPropVariantFromInt64(MFGetSystemTime(), &time));

	for (DWORD i = 0; i < count; i++)
	{
		wil::com_ptr_nothrow<IMFStreamDescriptor> desc;
		BOOL selected = FALSE;
		RETURN_IF_FAILED(pPresentationDescriptor->GetStreamDescriptorByIndex(i, &selected, &desc));

		DWORD id = 0;
		RETURN_IF_FAILED(desc->GetStreamIdentifier(&id));

		auto index = GetStreamIndexById(id);
		RETURN_HR_IF(E_FAIL, index < 0);

		BOOL thisSelected = FALSE;
		wil::com_ptr_nothrow<IMFStreamDescriptor> thisDesc;
		RETURN_IF_FAILED(_descriptor->GetStreamDescriptorByIndex(index, &thisSelected, &thisDesc));

		MF_STREAM_STATE state;
		RETURN_IF_FAILED(_streams[i]->GetStreamState(&state));
		if (thisSelected && state == MF_STREAM_STATE_STOPPED )
		{
			thisSelected = FALSE;
		}
		else if (!thisSelected && state != MF_STREAM_STATE_STOPPED)
		{
			thisSelected = TRUE;
		}

		WINTRACE(L"MediaSource::Start stream[%i] selected:%i thisSelected:%i", index, selected, thisSelected);
		if (selected != thisSelected)
		{
			if (selected)
			{
				RETURN_IF_FAILED(_descriptor->SelectStream(index));

				wil::com_ptr_nothrow<IUnknown> unk;
				RETURN_IF_FAILED(_streams[index].copy_to(&unk));
				RETURN_IF_FAILED(_queue->QueueEventParamUnk(MENewStream, GUID_NULL, S_OK, unk.get()));

				wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
				wil::com_ptr_nothrow<IMFMediaType> type;
				RETURN_IF_FAILED(desc->GetMediaTypeHandler(&handler));
				RETURN_IF_FAILED(handler->GetCurrentMediaType(&type));

				RETURN_IF_FAILED(_streams[index]->Start(type.get()));
			}
			else
			{
				RETURN_IF_FAILED(_descriptor->DeselectStream(index));
				RETURN_IF_FAILED(_streams[index]->Stop());
			}
		}
	}

	RETURN_IF_FAILED(_queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &time));
	return S_OK;
}

STDMETHODIMP MediaSource::Stop()
{
	WINTRACE(L"MediaSource::Stop");
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_descriptor);

	wil::unique_prop_variant time;
	RETURN_IF_FAILED(InitPropVariantFromInt64(MFGetSystemTime(), &time));

	for (DWORD i = 0; i < _streams.size(); i++)
	{
		RETURN_IF_FAILED(_streams[i]->Stop());
		RETURN_IF_FAILED(_descriptor->DeselectStream(i));
	}

	RETURN_IF_FAILED(_queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &time));
	return S_OK;
}

// IMFMediaSourceEx
STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** ppAttributes)
{
	WINTRACE(L"MediaSource::GetSourceAttributes");
	RETURN_HR_IF_NULL(E_POINTER, ppAttributes);
	winrt::slim_lock_guard lock(_lock);

	RETURN_IF_FAILED(QueryInterface(IID_PPV_ARGS(ppAttributes)));
	return S_OK;
}

// IMFMediaSource2
STDMETHODIMP MediaSource::SetMediaType(DWORD dwStreamID, IMFMediaType* pMediaType)
{
	WINTRACE(L"MediaSource::SetMediaType dwStreamId:%u pMediaType:%p", dwStreamID, pMediaType);
	RETURN_HR_IF_NULL(E_POINTER, pMediaType);
	winrt::slim_lock_guard lock(_lock);

	TraceMFAttributes(pMediaType, L"MediaType");
	return S_OK;
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes)
{
	WINTRACE(L"MediaSource::GetStreamAttributes dwStreamIdentifier:%u", dwStreamIdentifier);
	RETURN_HR_IF_NULL(E_POINTER, ppAttributes);
	*ppAttributes = nullptr;
	winrt::slim_lock_guard lock(_lock);

	RETURN_HR_IF_MSG(E_FAIL, dwStreamIdentifier >= _streams.size(), "dwStreamIdentifier %u is invalid", dwStreamIdentifier);
	RETURN_IF_FAILED(_streams[dwStreamIdentifier].copy_to(ppAttributes));
	return S_OK;
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown* pManager)
{
	WINTRACE(L"MediaSource::SetD3DManager pManager:%p", pManager);
	RETURN_HR_IF_NULL(E_POINTER, pManager);
	winrt::slim_lock_guard lock(_lock);

	for (DWORD i = 0; i < _streams.size(); i++)
	{
		RETURN_IF_FAILED(_streams[i]->SetD3DManager(pManager));
	}
	return S_OK;
}

// IMFGetService
STDMETHODIMP MediaSource::GetService(REFGUID siid, REFIID iid, LPVOID* ppvObject)
{
	if (iid == __uuidof(IMFDeviceController) || iid == __uuidof(IMFDeviceController2))
		return MF_E_UNSUPPORTED_SERVICE;

	WINTRACE(L"MediaSource::GetService siid '%s' iid '%s' failed", GUID_ToStringW(siid).c_str(), GUID_ToStringW(iid).c_str());
	RETURN_HR(MF_E_UNSUPPORTED_SERVICE);
}

// IMFSampleAllocatorControl
STDMETHODIMP MediaSource::SetDefaultAllocator(DWORD dwOutputStreamID, IUnknown* pAllocator)
{
	WINTRACE(L"MediaSource::SetDefaultAllocator dwOutputStreamID:%u pAllocator:%p", dwOutputStreamID, pAllocator);
	RETURN_HR_IF_NULL(E_POINTER, pAllocator);
	winrt::slim_lock_guard lock(_lock);

	auto index = GetStreamIndexById(dwOutputStreamID);
	RETURN_HR_IF(E_FAIL, index < 0);

	RETURN_HR_IF_MSG(E_FAIL, index < 0 || (DWORD)index >= _streams.size(), "dwOutputStreamID %u is invalid, index:%i", dwOutputStreamID, index);
	RETURN_HR(_streams[index]->SetAllocator(pAllocator));
}

STDMETHODIMP MediaSource::GetAllocatorUsage(DWORD dwOutputStreamID, DWORD* pdwInputStreamID, MFSampleAllocatorUsage* peUsage)
{
	WINTRACE(L"MediaSource::GetAllocatorUsage dwOutputStreamID:%u pdwInputStreamID:%p peUsage:%p", dwOutputStreamID, pdwInputStreamID, peUsage);
	RETURN_HR_IF_NULL(E_POINTER, peUsage);
	RETURN_HR_IF_NULL(E_POINTER, pdwInputStreamID);
	winrt::slim_lock_guard lock(_lock);

	auto index = GetStreamIndexById(dwOutputStreamID);
	RETURN_HR_IF(E_FAIL, index < 0);

	RETURN_HR_IF_MSG(E_FAIL, index < 0 || (DWORD)index >= _streams.size(), "dwOutputStreamID %u is invalid, index:%i", dwOutputStreamID, index);
	*pdwInputStreamID = dwOutputStreamID;
	*peUsage = _streams[index]->GetAllocatorUsage();
	return S_OK;
}

// IKsControl
STDMETHODIMP_(NTSTATUS) MediaSource::KsProperty(PKSPROPERTY property, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	WINTRACE(L"MediaSource::KsProperty len:%u data:%p dataLength:%u", length, data, dataLength);
	RETURN_HR_IF_NULL(E_POINTER, property);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	WINTRACE(L"MediaSource::KsProperty prop:%s", PKSIDENTIFIER_ToString(property, length).c_str());
	RETURN_HR_IF(E_INVALIDARG, length < sizeof(KSPROPERTY));
	*bytesReturned = 0;

	// image controls; everything else (KSPROPSETID_Pin, KSPROPSETID_Topology, region of interest,
	// per-frame settings, extended camera controls) isn't something the PS3 Eye has
	if (property->Set == PROPSETID_VIDCAP_VIDEOPROCAMP)
		return ControlProperty(vcam::ControlGroup::ProcAmp, property, length, data, dataLength, bytesReturned);
	if (property->Set == PROPSETID_VIDCAP_CAMERACONTROL)
		return ControlProperty(vcam::ControlGroup::Camera, property, length, data, dataLength, bytesReturned);
	if (property->Set == PROPSETID_VIDCAP_VIDEOCONTROL)
		return VideoControlProperty(property, length, data, dataLength, bytesReturned);

	return kNotFound;
}

STDMETHODIMP_(NTSTATUS) MediaSource::KsMethod(PKSMETHOD method, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	WINTRACE(L"MediaSource::KsMethod len:%u data:%p dataLength:%u", length, data, dataLength);
	RETURN_HR_IF_NULL(E_POINTER, method);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	WINTRACE(L"MediaSource::KsMethod method:%s", PKSIDENTIFIER_ToString(method, length).c_str());

	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaSource::KsEvent(PKSEVENT evt, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	WINTRACE(L"MediaSource::KsEvent evt:%p len:%u data:%p dataLength:%u", evt, length, data, dataLength);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	WINTRACE(L"MediaSource::KsEvent event:%s", PKSIDENTIFIER_ToString(evt, length).c_str());
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
