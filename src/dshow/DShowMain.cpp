// PS3EyeDShow64.dll / PS3EyeDShow32.dll: COM entry points and registration of the DirectShow
// filter in the video input device category. Built twice (x64 and x86) because an app only
// loads filters of its own bitness; register the 32-bit one with SysWOW64\regsvr32.exe.
#include "Filter.h"

#include <new>
#include <string>

#include "../source/Config.h"

#pragma comment(lib, "strmiids")
#pragma comment(lib, "ole32")
#pragma comment(lib, "advapi32")

namespace vcam {

// 084fccac-c0dd-4365-b96c-1943daef644d (PS3EyeSetup's kDShowClsid must match)
const CLSID CLSID_PS3EyeFilter = { 0x084fccac, 0xc0dd, 0x4365, { 0xb9, 0x6c, 0x19, 0x43, 0xda, 0xef, 0x64, 0x4d } };
HMODULE gModule = nullptr;
std::atomic<long> gObjects{ 0 };

namespace {

const wchar_t* kClsidString = L"{084FCCAC-C0DD-4365-B96C-1943DAEF644D}";

class ClassFactory final : public IClassFactory
{
public:
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
	{
		if (!ppv)
			return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IClassFactory)
		{
			*ppv = static_cast<IClassFactory*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return 2; }   // static object
	STDMETHODIMP_(ULONG) Release() override { return 1; }

	STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
	{
		if (!ppv)
			return E_POINTER;
		*ppv = nullptr;
		if (outer)
			return CLASS_E_NOAGGREGATION;
		Filter* filter = new (std::nothrow) Filter();
		if (!filter)
			return E_OUTOFMEMORY;
		HRESULT hr = filter->QueryInterface(riid, ppv);
		filter->Release();
		return hr;
	}

	STDMETHODIMP LockServer(BOOL lock) override
	{
		if (lock)
			gObjects++;
		else
			gObjects--;
		return S_OK;
	}
};

ClassFactory gFactory;

// regsvr32 and PS3EyeSetup may or may not have COM initialised already
struct ComScope
{
	ComScope() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
	~ComScope()
	{
		if (SUCCEEDED(hr))
			CoUninitialize();
	}
	HRESULT hr;
};

HRESULT WriteString(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value)
{
	HKEY key = nullptr;
	LSTATUS st = RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
	if (st != ERROR_SUCCESS)
		return HRESULT_FROM_WIN32(st);
	st = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
	RegCloseKey(key);
	return HRESULT_FROM_WIN32(st);
}

} // namespace

} // namespace vcam

using namespace vcam;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		gModule = module;
		DisableThreadLibraryCalls(module);
	}
	return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = nullptr;
	if (clsid != CLSID_PS3EyeFilter)
		return CLASS_E_CLASSNOTAVAILABLE;
	return gFactory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow()
{
	return gObjects == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
	wchar_t path[MAX_PATH] = L"";
	GetModuleFileNameW(gModule, path, MAX_PATH);

	// HKLM\Software\Classes is redirected to WOW6432Node for the 32-bit build
	const std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidString;
	HRESULT hr = WriteString(HKEY_LOCAL_MACHINE, key, nullptr, kCameraName);
	if (SUCCEEDED(hr))
		hr = WriteString(HKEY_LOCAL_MACHINE, key + L"\\InprocServer32", nullptr, path);
	if (SUCCEEDED(hr))
		hr = WriteString(HKEY_LOCAL_MACHINE, key + L"\\InprocServer32", L"ThreadingModel", L"Both");
	if (FAILED(hr))
		return hr;

	ComScope com;
	IFilterMapper2* mapper = nullptr;
	hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, (void**)&mapper);
	if (FAILED(hr))
		return hr;

	REGPINTYPES type = { &MEDIATYPE_Video, &MEDIASUBTYPE_NULL };
	REGFILTERPINS2 pin{};
	pin.dwFlags = REG_PINFLAG_B_OUTPUT;
	pin.cInstances = 1;
	pin.nMediaTypes = 1;
	pin.lpMediaType = &type;
	pin.clsPinCategory = &PIN_CATEGORY_CAPTURE;

	REGFILTER2 filter{};
	filter.dwVersion = 2;
	filter.dwMerit = MERIT_DO_NOT_USE;
	filter.cPins2 = 1;
	filter.rgPins2 = &pin;

	hr = mapper->RegisterFilter(CLSID_PS3EyeFilter, kCameraName, nullptr, &CLSID_VideoInputDeviceCategory, nullptr, &filter);
	mapper->Release();
	return hr;
}

STDAPI DllUnregisterServer()
{
	{
		ComScope com;
		IFilterMapper2* mapper = nullptr;
		if (SUCCEEDED(CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, (void**)&mapper)))
		{
			mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr, CLSID_PS3EyeFilter);
			mapper->Release();
		}
	}
	LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"Software\\Classes\\CLSID\\") + kClsidString).c_str());
	return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(st);
}
