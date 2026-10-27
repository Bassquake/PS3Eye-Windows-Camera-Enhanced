// PS3EyeSetup.exe: installs, tests, configures and removes the "PS3 Eye Camera" virtual camera.
// Double-click for a menu, or run with a command:
//   install | uninstall | status | test | settings | set <Name> <Value> | reset | log
//
// Windows 11: a Media Foundation virtual camera (PS3EyeVCam.dll, loaded by the Frame Server).
// Windows 10 has no virtual camera API, so there it registers a DirectShow capture filter
// (PS3EyeDShow64.dll / PS3EyeDShow32.dll) fed by PS3EyeServer.exe.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dshow.h>
#include <cfgmgr32.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfvirtualcamera.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <devicetopology.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iostream>
#include <string>
#include <vector>

#include "../core/convert.h"
#include "../core/ps3eye_camera.h"
#include "../source/Config.h"
#include "usbdriver.h"

#pragma comment(lib, "mf")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "ole32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "advapi32")
#pragma comment(lib, "winmm")

namespace {

const wchar_t* kClsid = L"{C4D641C7-CF26-435F-849B-56F2A715F55E}"; // must match dllmain.cpp
const wchar_t* kDShowClsid = L"{084FCCAC-C0DD-4365-B96C-1943DAEF644D}"; // must match DShowMain.cpp
const wchar_t* kVideoInputCategory = L"{860BB310-5D01-11D0-BD3B-00A0C911CE86}";
const wchar_t* kDllName = L"PS3EyeVCam.dll";
const wchar_t* kDShow64 = L"PS3EyeDShow64.dll";
const wchar_t* kDShow32 = L"PS3EyeDShow32.dll";
const wchar_t* kServerExe = L"PS3EyeServer.exe";
const wchar_t* kExeName = L"PS3EyeSetup.exe";
const wchar_t* kAllFiles[] = { kExeName, kDllName, kDShow64, kDShow32, kServerExe };

HANDLE gConsole = GetStdHandle(STD_OUTPUT_HANDLE);

void Color(WORD c) { SetConsoleTextAttribute(gConsole, c); }
void Ok(const std::wstring& s) { Color(FOREGROUND_GREEN | FOREGROUND_INTENSITY); wprintf(L"  [OK] "); Color(7); wprintf(L"%s\n", s.c_str()); }
void Bad(const std::wstring& s) { Color(FOREGROUND_RED | FOREGROUND_INTENSITY); wprintf(L"  [!!] "); Color(7); wprintf(L"%s\n", s.c_str()); }
void Info(const std::wstring& s) { Color(FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY); wprintf(L"  [..] "); Color(7); wprintf(L"%s\n", s.c_str()); }
void Title(const std::wstring& s) { Color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY); wprintf(L"\n%s\n", s.c_str()); Color(7); }

std::wstring Widen(const char* s)
{
	std::wstring w;
	while (*s)
		w.push_back((wchar_t)(unsigned char)*s++);
	return w;
}

std::wstring HrText(HRESULT hr)
{
	wchar_t buf[512] = L"";
	FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, hr, 0, buf, 512, nullptr);
	std::wstring s = buf;
	while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' '))
		s.pop_back();
	wchar_t code[32];
	swprintf_s(code, L" (0x%08X)", (unsigned)hr);
	return s + code;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
	PWSTR p = nullptr;
	std::wstring s;
	if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p)
		s = p;
	CoTaskMemFree(p);
	return s;
}

std::wstring InstallDir() { return KnownFolder(FOLDERID_ProgramFiles) + L"\\PS3EyeVCam"; }

std::wstring ExeDir()
{
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	std::wstring s = path;
	return s.substr(0, s.find_last_of(L'\\'));
}

bool FileExists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---- virtual camera API (Windows 11 only, so loaded dynamically) ------------

using PFN_MFCreateVirtualCamera = HRESULT(WINAPI*)(MFVirtualCameraType, MFVirtualCameraLifetime, MFVirtualCameraAccess,
	LPCWSTR, LPCWSTR, const GUID*, ULONG, IMFVirtualCamera**);

PFN_MFCreateVirtualCamera VirtualCameraApi()
{
	HMODULE m = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	return m ? (PFN_MFCreateVirtualCamera)GetProcAddress(m, "MFCreateVirtualCamera") : nullptr;
}

HRESULT OpenVirtualCamera(IMFVirtualCamera** cam)
{
	auto create = VirtualCameraApi();
	if (!create)
		return HRESULT_FROM_WIN32(ERROR_OLD_WIN_VERSION);
	return create(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System, MFVirtualCameraAccess_AllUsers,
		vcam::kCameraName, kClsid, nullptr, 0, cam);
}

HRESULT RemoveVirtualCamera()
{
	IMFVirtualCamera* cam = nullptr;
	HRESULT hr = OpenVirtualCamera(&cam);
	if (SUCCEEDED(hr))
	{
		hr = cam->Remove();
		cam->Release();
	}
	return hr;
}

bool VirtualCameraVisible()
{
	IMFAttributes* attrs = nullptr;
	if (FAILED(MFCreateAttributes(&attrs, 1)))
		return false;
	attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

	IMFActivate** devices = nullptr;
	UINT32 count = 0;
	bool found = false;
	if (SUCCEEDED(MFEnumDeviceSources(attrs, &devices, &count)))
	{
		for (UINT32 i = 0; i < count; i++)
		{
			WCHAR* name = nullptr;
			UINT32 len = 0;
			if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len)))
			{
				found |= wcscmp(name, vcam::kCameraName) == 0;
				CoTaskMemFree(name);
			}
			devices[i]->Release();
		}
		CoTaskMemFree(devices);
	}
	attrs->Release();
	return found;
}

std::wstring RegisteredDllPath()
{
	std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + kClsid + L"\\InprocServer32";
	wchar_t path[MAX_PATH] = L"";
	DWORD size = sizeof(path);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS)
		return L"";
	return path;
}

HRESULT CallDllExport(const std::wstring& dll, const char* name)
{
	HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!m)
		return HRESULT_FROM_WIN32(GetLastError());
	auto fn = (HRESULT(STDAPICALLTYPE*)())GetProcAddress(m, name);
	HRESULT hr = fn ? fn() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
	FreeLibrary(m);
	return hr;
}

// Copies a file even if the old copy is loaded by the Windows camera service.
bool InstallFile(const std::wstring& src, const std::wstring& dst)
{
	if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
	{
		std::wstring old = dst + L".old" + std::to_wstring(GetTickCount64());
		if (!MoveFileExW(dst.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING))
			return false;
		MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
		if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
			return false;
	}
	DeleteFileW((dst + L":Zone.Identifier").c_str()); // drop "downloaded from the internet" mark
	return true;
}

void RemoveFileNowOrLater(const std::wstring& p)
{
	if (!DeleteFileW(p.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
		MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

DWORD RunHidden(std::wstring cmd)
{
	STARTUPINFOW si{ sizeof(si) };
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		return GetLastError();
	WaitForSingleObject(pi.hProcess, 30000);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return code;
}

// ---- DirectShow camera (Windows 10) ------------------------------------------

bool UseDirectShow() { return VirtualCameraApi() == nullptr; }

// Path of the registered filter DLL in the 64-bit or 32-bit registry view, "" if none.
std::wstring RegisteredDShowPath(bool wow32)
{
	std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + kDShowClsid + L"\\InprocServer32";
	wchar_t path[MAX_PATH] = L"";
	DWORD size = sizeof(path);
	DWORD flags = RRF_RT_REG_SZ | (wow32 ? RRF_SUBKEY_WOW6432KEY : RRF_SUBKEY_WOW6464KEY);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, flags, nullptr, path, &size) != ERROR_SUCCESS)
		return L"";
	return path;
}

// The 32-bit DLL can't be loaded here, so the 32-bit regsvr32 does it.
HRESULT Regsvr32(const std::wstring& dll, bool unregister)
{
	wchar_t dir[MAX_PATH] = L"";
	if (!GetSystemWow64DirectoryW(dir, MAX_PATH))
		return HRESULT_FROM_WIN32(GetLastError());
	DWORD code = RunHidden(L"\"" + std::wstring(dir) + L"\\regsvr32.exe\" /s " + (unregister ? L"/u " : L"") + L"\"" + dll + L"\"");
	return code == 0 ? S_OK : E_FAIL;
}

// Removes whatever DllUnregisterServer may have left, in both registry views.
void DeleteDShowKeys()
{
	for (REGSAM view : { KEY_WOW64_64KEY, KEY_WOW64_32KEY })
	{
		HKEY classes = nullptr;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID", 0, KEY_ALL_ACCESS | view, &classes) != ERROR_SUCCESS)
			continue;
		RegDeleteTreeW(classes, kDShowClsid);
		RegDeleteTreeW(classes, (std::wstring(kVideoInputCategory) + L"\\Instance\\" + kDShowClsid).c_str());
		RegCloseKey(classes);
	}
}

void StopServer()
{
	RunHidden(std::wstring(L"taskkill.exe /f /im ") + kServerExe);
}

bool ServerRunning()
{
	HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\PS3EyeVCam.Server"); // see SharedFrames.cpp
	if (m)
		CloseHandle(m);
	return m != nullptr;
}

// Whether the camera is in the DirectShow video device list (as 64-bit apps see it).
bool DShowCameraVisible()
{
	ICreateDevEnum* devEnum = nullptr;
	if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void**)&devEnum)))
		return false;
	bool found = false;
	IEnumMoniker* monikers = nullptr;
	if (devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0) == S_OK)
	{
		IMoniker* m = nullptr;
		while (!found && monikers->Next(1, &m, nullptr) == S_OK)
		{
			IPropertyBag* bag = nullptr;
			if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)&bag)))
			{
				VARIANT v;
				VariantInit(&v);
				if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR)
					found = wcscmp(v.bstrVal, vcam::kCameraName) == 0;
				VariantClear(&v);
				bag->Release();
			}
			m->Release();
		}
		monikers->Release();
	}
	devEnum->Release();
	return found;
}

// ---- microphone watcher (scheduled task) ----------------------------------------
// The PS3 Eye's mic sometimes fails to start when it's plugged in or Windows boots (see
// usbdriver::RepairMic). A task running "PS3EyeSetup micwatch" as SYSTEM from startup sets it
// up again whenever that happens.

const wchar_t* kMicTaskName = L"PS3 Eye Camera microphone fix";

std::wstring MicTaskArg() { return std::wstring(L" /tn \"") + kMicTaskName + L"\""; }

bool MicTaskInstalled()
{
	return RunHidden(L"schtasks.exe /query" + MicTaskArg()) == 0;
}

void StopMicTask()
{
	RunHidden(L"schtasks.exe /end" + MicTaskArg());
}

void RemoveMicTask()
{
	StopMicTask();
	RunHidden(L"schtasks.exe /delete /f" + MicTaskArg());
}

// Creates (or replaces) the task for 'exe' and starts it now.
bool InstallMicTask(const std::wstring& exe, std::wstring& error)
{
	std::wstring xml =
		L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
		L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
		L"  <RegistrationInfo><Description>Sets the PS3 Eye microphone up again when it fails to start (PS3 Eye Camera).</Description></RegistrationInfo>\r\n"
		L"  <Triggers><BootTrigger><Enabled>true</Enabled></BootTrigger></Triggers>\r\n"
		L"  <Principals><Principal id=\"System\"><UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
		L"  <Settings>\r\n"
		L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
		L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
		L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
		L"    <StartWhenAvailable>true</StartWhenAvailable>\r\n"
		L"    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>\r\n"
		L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
		L"    <Priority>7</Priority>\r\n"
		L"    <RestartOnFailure><Interval>PT1M</Interval><Count>3</Count></RestartOnFailure>\r\n"
		L"  </Settings>\r\n"
		L"  <Actions Context=\"System\"><Exec><Command>\"" + exe + L"\"</Command><Arguments>micwatch</Arguments></Exec></Actions>\r\n"
		L"</Task>\r\n";

	wchar_t tmp[MAX_PATH] = L"";
	GetTempPathW(MAX_PATH, tmp);
	const std::wstring path = std::wstring(tmp) + L"PS3EyeMicTask.xml";
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"wb") || !f)
	{
		error = L"can't write " + path;
		return false;
	}
	const wchar_t bom = 0xFEFF; // schtasks wants UTF-16
	fwrite(&bom, sizeof(bom), 1, f);
	fwrite(xml.c_str(), sizeof(wchar_t), xml.size(), f);
	fclose(f);

	StopMicTask();
	DWORD code = RunHidden(L"schtasks.exe /create /f /xml \"" + path + L"\"" + MicTaskArg());
	DeleteFileW(path.c_str());
	if (code != 0)
	{
		error = L"schtasks /create failed (exit code " + std::to_wstring(code) + L")";
		return false;
	}
	RunHidden(L"schtasks.exe /run" + MicTaskArg());
	return true;
}

DWORD CALLBACK OnUsbArrival(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA, DWORD)
{
	if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL)
		SetEvent((HANDLE)context);
	return ERROR_SUCCESS;
}

// "micwatch": runs forever (as SYSTEM, from the scheduled task).
int MicWatch()
{
	vcam::Log(L"Microphone watcher started");

	// woken by any USB device arriving, and every 15 s regardless (a restart of just the mic
	// isn't a USB arrival)
	const GUID usbDevice = { 0xA5DCBF10, 0x6530, 0x11D2, { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED } }; // GUID_DEVINTERFACE_USB_DEVICE
	HANDLE arrived = CreateEventW(nullptr, FALSE, TRUE, nullptr); // signalled: check once at start
	CM_NOTIFY_FILTER filter{};
	filter.cbSize = sizeof(filter);
	filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
	filter.u.DeviceInterface.ClassGuid = usbDevice;
	HCMNOTIFICATION notification = nullptr;
	if (CM_Register_Notification(&filter, arrived, OnUsbArrival, &notification) != CR_SUCCESS)
		vcam::Log(L"Microphone watcher: no plug-in notifications; checking every 15 s");

	ULONGLONG gaveUpAt = 0;
	for (;;)
	{
		const bool plugged = WaitForSingleObject(arrived, 15000) == WAIT_OBJECT_0;
		if (plugged)
			Sleep(5000); // let Windows finish starting the camera's interfaces

		// keep the names on (a camera on a new USB port, or a mic node a repair re-created)
		std::wstring error;
		usbdriver::SetDeviceNames(false, error);

		// only the "audio driver failed to start" case: a wrong driver is Install's business
		if (usbdriver::Mic() != usbdriver::MicState::NotStarted)
			continue;
		if (!plugged && gaveUpAt && GetTickCount64() - gaveUpAt < 10 * 60 * 1000)
			continue; // failed recently: wait for a replug or 10 minutes

		switch (usbdriver::RepairMic(error))
		{
		case usbdriver::Result::Installed:
			vcam::Log(L"Microphone watcher: the mic had failed to start; set it up again");
			usbdriver::SetDeviceNames(false, error);
			gaveUpAt = 0;
			break;
		case usbdriver::Result::Failed:
			vcam::Log(L"Microphone watcher: couldn't start the mic: %s", error.c_str());
			gaveUpAt = GetTickCount64();
			break;
		default:
			break;
		}
	}
}

void UnregisterDShow()
{
	StopServer();
	std::wstring dll64 = RegisteredDShowPath(false), dll32 = RegisteredDShowPath(true);
	if (dll64.empty())
		dll64 = InstallDir() + L"\\" + kDShow64;
	if (dll32.empty())
		dll32 = InstallDir() + L"\\" + kDShow32;
	if (FileExists(dll64))
		CallDllExport(dll64, "DllUnregisterServer");
	if (FileExists(dll32))
		Regsvr32(dll32, true);
	DeleteDShowKeys();
}

void DriverHint(ps3eye::Status st)
{
	if (st == ps3eye::Status::NoDriver)
	{
		Info(L"Choose Install (1): it sets up the USB driver automatically.");
	}
	else if (st == ps3eye::Status::AccessDenied || st == ps3eye::Status::Busy)
	{
		Info(L"Another program is using the camera right now (that's normal during a call).");
	}
	else if (st == ps3eye::Status::NotFound)
	{
		Info(L"Plug the PS3 Eye into a USB port directly on the PC (avoid unpowered hubs).");
	}
}

// The microphone array is plain USB audio, handled by Windows. A leftover Zadig driver on it
// (often "USB Serial (CDC)") hides it from apps, so swap that back to Windows' audio driver.
// Never fails the install: the camera works either way.
void RepairMic()
{
	std::wstring error;
	switch (usbdriver::RepairMic(error))
	{
	case usbdriver::Result::AlreadyInstalled:
		Ok(L"Microphone uses Windows' USB audio driver");
		break;
	case usbdriver::Result::Installed:
		Ok(L"Microphone fixed: Windows set it up again with its USB audio driver");
		vcam::Log(L"Microphone: re-detected with usbaudio (wrong driver or failed start)");
		break;
	case usbdriver::Result::NotPlugged:
		break;
	case usbdriver::Result::Failed:
		Bad(L"Microphone: " + error);
		Info(L"The camera still works. For the mic: Device Manager > the camera's Interface 1 > Uninstall device,");
		Info(L"  tick \"Delete the driver software\", then Action > Scan for hardware changes.");
		break;
	}
}

// Waits for the camera, installs WinUSB on it if needed, and checks libusb can open it.
bool InstallUsbDriver()
{
	if (ps3eye::Camera::Count() == 0)
	{
		Info(L"Plug in the PS3 Eye now (waiting up to 60 seconds)...");
		for (int i = 0; i < 120 && ps3eye::Camera::Count() == 0; i++)
			Sleep(500);
		if (ps3eye::Camera::Count() == 0)
		{
			Bad(L"No PS3 Eye found. Plug it straight into the PC and run Install again.");
			return false;
		}
		Sleep(2000); // let Windows finish enumerating it
	}

	std::wstring error;
	switch (usbdriver::Install(error))
	{
	case usbdriver::Result::AlreadyInstalled:
		Ok(L"USB driver already installed (" + usbdriver::CurrentService() + L")");
		break;
	case usbdriver::Result::Installed:
		Ok(L"USB driver installed (Windows' built-in WinUSB)");
		vcam::Log(L"WinUSB installed on the camera");
		break;
	case usbdriver::Result::NotPlugged:
		Bad(L"The camera disappeared. Plug it back in and run Install again.");
		return false;
	case usbdriver::Result::Failed:
		Bad(L"USB driver install failed: " + error);
		Info(L"Manual fix: Device Manager > USB Camera-B4.09.24.1 > Update driver > Browse my computer >");
		Info(L"  Let me pick > Universal Serial Bus devices > WinUsb Device. Then run Install again.");
		return false;
	}

	// names instead of "USB Camera-B4.09.24.1"; set before the mic repair so a restarted mic
	// starts with its name, and again after in case the repair had to re-create the device
	std::wstring nameError;
	usbdriver::SetDeviceNames(false, nameError);
	RepairMic();
	if (usbdriver::SetDeviceNames(false, nameError))
		Ok(L"Device names: \"PS3 Eye Camera\" and \"PS3 Eye Microphone\"");
	else
		Info(L"Couldn't rename the camera in Device Manager: " + nameError);

	// WinUSB needs a moment to start after a fresh install
	ps3eye::Status st = ps3eye::Status::NoDriver;
	for (int i = 0; i < 20; i++)
	{
		st = ps3eye::Camera::Probe(vcam::LoadConfig().deviceIndex);
		if (st == ps3eye::Status::Ok || st == ps3eye::Status::Busy || st == ps3eye::Status::AccessDenied)
			break;
		Sleep(500);
	}
	if (st == ps3eye::Status::Ok || st == ps3eye::Status::Busy || st == ps3eye::Status::AccessDenied)
	{
		Ok(L"Camera responds over USB");
		return true;
	}
	Bad(Widen(ps3eye::StatusText(st)));
	Info(L"Unplug the camera, plug it back in and run Install again. If it still fails, restart Windows.");
	return false;
}

// ---- commands ----------------------------------------------------------------

int Status()
{
	Title(L"Status");
	int problems = 0;

	if (UseDirectShow())
	{
		Ok(L"Windows 10 mode: DirectShow camera (Zoom, Teams, Discord, OBS, browsers)");

		std::wstring dll64 = RegisteredDShowPath(false), dll32 = RegisteredDShowPath(true);
		if (!dll64.empty() && FileExists(dll64))
			Ok(L"Camera driver registered for 64-bit apps: " + dll64);
		else
		{
			Bad(L"Camera driver not installed (choose Install)");
			problems++;
		}
		if (!dll32.empty() && FileExists(dll32))
			Ok(L"Camera driver registered for 32-bit apps");
		else
		{
			Bad(L"Not registered for 32-bit apps (choose Install)");
			problems++;
		}

		if (DShowCameraVisible())
			Ok(std::wstring(L"\"") + vcam::kCameraName + L"\" is visible to apps");
		else
		{
			Bad(std::wstring(L"\"") + vcam::kCameraName + L"\" is not in the camera list yet");
			problems++;
		}

		if (ServerRunning())
			Info(L"Camera in use by an app right now (PS3EyeServer is running)");
	}
	else
	{
		Ok(L"Windows 11 virtual camera support");

		std::wstring dll = RegisteredDllPath();
		if (!dll.empty() && FileExists(dll))
			Ok(L"Camera driver registered: " + dll);
		else
		{
			Bad(L"Camera driver not installed (choose Install)");
			problems++;
		}

		if (VirtualCameraVisible())
			Ok(std::wstring(L"\"") + vcam::kCameraName + L"\" is visible to apps");
		else
		{
			Bad(std::wstring(L"\"") + vcam::kCameraName + L"\" is not in the camera list yet");
			problems++;
		}
	}

	int n = ps3eye::Camera::Count();
	if (n > 0)
		Ok(L"PS3 Eye connected (" + std::to_wstring(n) + L" found)");
	else
	{
		Bad(L"No PS3 Eye on USB");
		DriverHint(ps3eye::Status::NotFound);
		problems++;
	}

	if (n > 0)
	{
		auto st = ps3eye::Camera::Probe(vcam::LoadConfig().deviceIndex);
		if (st == ps3eye::Status::Ok)
			Ok(L"USB driver (WinUSB) installed");
		else if (st == ps3eye::Status::AccessDenied || st == ps3eye::Status::Busy)
		{
			Ok(L"USB driver installed; camera currently in use");
		}
		else
		{
			if (st == ps3eye::Status::NoDriver)
				Bad(L"USB driver not installed yet (choose Install)");
			else
				Bad(Widen(ps3eye::StatusText(st)));
			if (st != ps3eye::Status::NoDriver)
				DriverHint(st);
			problems++;
		}

		std::wstring micService;
		usbdriver::MicState mic = usbdriver::Mic(&micService);
		if (mic == usbdriver::MicState::WrongDriver)
		{
			Bad(L"Microphone has the wrong driver (" + (micService.empty() ? std::wstring(L"none") : micService) + L"), so apps can't use it (choose Install)");
			problems++;
		}
		else if (mic == usbdriver::MicState::NotStarted)
		{
			Bad(L"Microphone failed to start (Device Manager error 10); choose Install to set it up again");
			problems++;
		}
		else if (mic == usbdriver::MicState::Ok)
		{
			Ok(L"Microphone available to apps (Windows USB audio)");
		}

		if (usbdriver::DeviceNamesSet())
			Ok(L"Named \"PS3 Eye Camera\" and \"PS3 Eye Microphone\" in Device Manager");
		else
			Info(L"Device Manager shows the default names (USB Camera-B4.09.24.1); Install renames them");

		if (MicTaskInstalled())
			Ok(L"Microphone auto-fix task installed");
		else
		{
			Bad(L"Microphone auto-fix task missing (choose Install)");
			problems++;
		}
	}

	Info(L"Log: " + vcam::LogPath());
	return problems;
}

int Install()
{
	Title(L"Installing");
	const bool dshow = UseDirectShow();
	if (dshow)
		Info(L"Windows 10: installing the DirectShow camera (the Windows 11 virtual camera API isn't available)");

	const std::wstring srcDir = ExeDir(), dstDir = InstallDir();
	std::vector<const wchar_t*> required = { kExeName };
	if (dshow)
		required.insert(required.end(), { kDShow64, kDShow32, kServerExe });
	else
		required.push_back(kDllName);
	for (const wchar_t* f : required)
	{
		if (!FileExists(srcDir + L"\\" + f))
		{
			Bad(std::wstring(L"Can't find ") + f + L" next to this program. Keep all the files in the same folder.");
			return 1;
		}
	}

	// 1. files -> Program Files (the camera service can't read files inside your user folder)
	CreateDirectoryW(dstDir.c_str(), nullptr);
	if (_wcsicmp(srcDir.c_str(), dstDir.c_str()) != 0)
	{
		// release the old files before replacing them
		StopMicTask(); // it runs PS3EyeSetup.exe
		if (dshow)
			StopServer();
		else
			RemoveVirtualCamera();
		for (const wchar_t* f : kAllFiles)
		{
			if (FileExists(srcDir + L"\\" + f) && !InstallFile(srcDir + L"\\" + f, dstDir + L"\\" + f))
			{
				Bad(L"Could not copy files to " + dstDir + L": " + HrText(HRESULT_FROM_WIN32(GetLastError())));
				return 1;
			}
		}
	}
	Ok(L"Files copied to " + dstDir);

	// 2. log folder writable by the camera service (LOCAL SERVICE) and users
	std::wstring data = vcam::DataFolder();
	CreateDirectoryW(data.c_str(), nullptr);
	RunHidden(L"icacls.exe \"" + data + L"\" /grant *S-1-5-19:(OI)(CI)M *S-1-5-32-545:(OI)(CI)M /Q");
	Ok(L"Log folder ready: " + data);

	// microphone watcher: fixes the mic when it fails to start after a plug-in or reboot
	std::wstring taskError;
	if (InstallMicTask(dstDir + L"\\" + kExeName, taskError))
		Ok(L"Microphone auto-fix task installed (runs at startup)");
	else
		Bad(L"Microphone auto-fix task: " + taskError + L" (if the mic stops after a replug, run Install)");

	// settings key must exist so the camera can watch it for live changes
	HKEY settingsKey = nullptr;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, vcam::kRegKey, 0, nullptr, 0, KEY_READ, nullptr, &settingsKey, nullptr) == ERROR_SUCCESS)
		RegCloseKey(settingsKey);

	if (dshow)
	{
		// 3. register the capture filter for 64-bit and 32-bit apps
		HRESULT hr = CallDllExport(dstDir + L"\\" + kDShow64, "DllRegisterServer");
		if (FAILED(hr))
		{
			Bad(L"Registering the camera for 64-bit apps failed: " + HrText(hr));
			return 1;
		}
		hr = Regsvr32(dstDir + L"\\" + kDShow32, false);
		if (FAILED(hr))
		{
			Bad(L"Registering the camera for 32-bit apps failed");
			return 1;
		}
		Ok(std::wstring(L"\"") + vcam::kCameraName + L"\" added to Windows (64-bit and 32-bit apps)");
		vcam::Log(L"Installed (DirectShow) from %s", srcDir.c_str());

		// 4. USB driver
		if (!InstallUsbDriver())
			return 2;

		Ok(L"All done. Pick \"PS3 Eye Camera\" in Zoom, Discord, Teams, OBS or your browser.");
		Info(L"On Windows 10 the built-in Camera app only shows real webcams, so it won't list it.");
		return 0;
	}

	const std::wstring dstDll = dstDir + L"\\" + kDllName;

	// a DirectShow camera from before an upgrade to Windows 11 would show up twice
	if (!RegisteredDShowPath(false).empty() || !RegisteredDShowPath(true).empty())
		UnregisterDShow();

	// 3. COM registration
	HRESULT hr = CallDllExport(dstDll, "DllRegisterServer");
	if (FAILED(hr))
	{
		Bad(L"Registering the driver failed: " + HrText(hr));
		return 1;
	}
	Ok(L"Driver registered");

	// 4. the virtual camera itself (system-wide, survives reboots)
	RemoveVirtualCamera();
	IMFVirtualCamera* cam = nullptr;
	hr = OpenVirtualCamera(&cam);
	if (SUCCEEDED(hr))
	{
		hr = cam->Start(nullptr);
		cam->Release();
	}
	if (FAILED(hr))
	{
		Bad(L"Creating the virtual camera failed: " + HrText(hr));
		if (hr == E_ACCESSDENIED)
			Info(L"Run this program as administrator and make sure the Windows Camera Frame Server service is not disabled.");
		return 1;
	}
	Ok(std::wstring(L"\"") + vcam::kCameraName + L"\" added to Windows");
	vcam::Log(L"Installed from %s", srcDir.c_str());

	// 5. USB driver: bind Windows' built-in WinUSB to the camera (replaces Zadig).
	// Last, so a missing camera doesn't block the rest; exit code 2 tells the installer.
	if (!InstallUsbDriver())
		return 2;

	Ok(L"All done. Pick \"PS3 Eye Camera\" in Zoom, Discord, Teams or the Camera app.");
	return 0;
}

int Uninstall()
{
	Title(L"Uninstalling");
	RemoveMicTask();
	Ok(L"Microphone auto-fix task removed");
	if (VirtualCameraApi())
	{
		HRESULT hr = RemoveVirtualCamera();
		if (SUCCEEDED(hr))
			Ok(L"Virtual camera removed");
		else
			Info(L"Virtual camera removal: " + HrText(hr));
	}

	UnregisterDShow();
	if (UseDirectShow())
		Ok(L"Camera removed from the DirectShow device list");

	std::wstring dll = RegisteredDllPath();
	if (dll.empty())
		dll = InstallDir() + L"\\" + kDllName;
	if (FileExists(dll))
		CallDllExport(dll, "DllUnregisterServer");
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"Software\\Classes\\CLSID\\") + kClsid).c_str());
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, vcam::kRegKey);
	Ok(L"Driver unregistered and settings removed");

	const std::wstring dir = InstallDir();
	for (const wchar_t* f : kAllFiles)
		RemoveFileNowOrLater(dir + L"\\" + f);
	if (!RemoveDirectoryW(dir.c_str()))
		MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
	Ok(L"Files removed (anything in use goes at next reboot)");

	std::wstring error;
	if (usbdriver::SetDeviceNames(true, error))
		Ok(L"Device Manager names restored");
	if (usbdriver::Remove(error))
		Ok(L"USB driver removed (camera is back to how Windows found it)");
	else
		Info(L"USB driver removal: " + error);

	if (!UseDirectShow() && VirtualCameraVisible())
		Info(L"Still listed? Device Manager > Cameras > PS3 Eye Camera > Uninstall device.");
	return 0;
}

// Where the camera and microphone tests save their snapshot and recording:
// Documents\PS3 Eye Camera Tests (created on first use), or the temp folder if that fails.
std::wstring TestFolder()
{
	std::wstring docs = KnownFolder(FOLDERID_Documents);
	if (!docs.empty())
	{
		std::wstring dir = docs + L"\\PS3 Eye Camera Tests";
		if (CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
			return dir;
	}
	wchar_t tmp[MAX_PATH] = L"";
	GetTempPathW(MAX_PATH, tmp);
	std::wstring dir = tmp;
	if (!dir.empty() && dir.back() == L'\\')
		dir.pop_back();
	return dir;
}

// Shows the new file selected in File Explorer, reusing a window that already shows its folder.
void ShowInFolder(const std::wstring& file)
{
	PIDLIST_ABSOLUTE item = ILCreateFromPathW(file.c_str());
	HRESULT hr = item ? SHOpenFolderAndSelectItems(item, 0, nullptr, 0) : E_FAIL;
	if (item)
		ILFree(item);
	if (FAILED(hr)) // always opens a new window
		ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + file + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

bool SaveBmp(const std::wstring& path, const std::vector<uint8_t>& bgrx, int w, int h)
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"wb") || !f)
		return false;
	const int rowBytes = (w * 3 + 3) & ~3;
	BITMAPFILEHEADER fh{};
	BITMAPINFOHEADER ih{};
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(fh) + sizeof(ih);
	fh.bfSize = fh.bfOffBits + rowBytes * h;
	ih.biSize = sizeof(ih);
	ih.biWidth = w;
	ih.biHeight = h; // bottom-up
	ih.biPlanes = 1;
	ih.biBitCount = 24;
	fwrite(&fh, sizeof(fh), 1, f);
	fwrite(&ih, sizeof(ih), 1, f);
	std::vector<uint8_t> row(rowBytes, 0);
	for (int y = h - 1; y >= 0; y--)
	{
		for (int x = 0; x < w; x++)
			memcpy(&row[x * 3], &bgrx[((size_t)y * w + x) * 4], 3);
		fwrite(row.data(), 1, rowBytes, f);
	}
	fclose(f);
	return true;
}

int Test()
{
	Title(L"Camera test (3 seconds at 640x480, 60 fps)");
	vcam::Config cfg = vcam::LoadConfig();
	ps3eye::Camera cam;
	auto st = cam.Start(640, 480, 60, cfg.camera, cfg.deviceIndex);
	if (st != ps3eye::Status::Ok)
	{
		Bad(Widen(ps3eye::StatusText(st)));
		DriverHint(st);
		if (st == ps3eye::Status::AccessDenied || st == ps3eye::Status::Busy)
			Info(L"Close apps using \"PS3 Eye Camera\" (Zoom, Discord, Camera app, browser tabs) and retry.");
		return 1;
	}

	std::vector<uint8_t> bayer, bgrx(640 * 480 * 4);
	int frames = 0;
	auto start = std::chrono::steady_clock::now();
	while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3))
	{
		if (cam.WaitFrame(bayer, std::chrono::milliseconds(500)))
			frames++;
		if (cam.DeviceLost())
			break;
	}
	double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	bool lost = cam.DeviceLost();
	cam.Stop();

	if (frames == 0)
	{
		Bad(lost ? L"Camera disconnected during the test." : L"No frames received. Try another USB port (USB 2.0 ports work best).");
		return 1;
	}

	wchar_t line[128];
	swprintf_s(line, L"%d frames in %.1f s = %.1f fps", frames, secs, frames / secs);
	(frames / secs > 50 ? Ok : Bad)(line);

	ps3eye::BayerToBGRX(bayer.data(), 640, 480, bgrx.data(), 640 * 4);
	std::wstring snap = TestFolder() + L"\\ps3eye_snapshot.bmp";
	if (SaveBmp(snap, bgrx, 640, 480))
	{
		Ok(L"Snapshot saved: " + snap);
		ShowInFolder(snap);
	}
	return 0;
}

// ---- microphone test ----------------------------------------------------------------

// The PS3 Eye's recording endpoint, found through the USB device behind it (so any name works).
IMMDevice* FindMicEndpoint(std::wstring* name)
{
	IMMDeviceEnumerator* en = nullptr;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))))
		return nullptr;
	IMMDevice* found = nullptr;
	IMMDeviceCollection* list = nullptr;
	if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &list)))
	{
		UINT n = 0;
		list->GetCount(&n);
		for (UINT i = 0; i < n && !found; i++)
		{
			IMMDevice* d = nullptr;
			if (FAILED(list->Item(i, &d)))
				continue;
			IDeviceTopology* topo = nullptr;
			IConnector* conn = nullptr;
			LPWSTR id = nullptr;
			if (SUCCEEDED(d->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, (void**)&topo)) &&
				SUCCEEDED(topo->GetConnector(0, &conn)) && SUCCEEDED(conn->GetDeviceIdConnectedTo(&id)) && id)
			{
				std::wstring s = id;
				for (auto& c : s)
					c = (wchar_t)towupper(c);
				if (s.find(L"VID_1415&PID_2000") != std::wstring::npos)
				{
					found = d;
					found->AddRef();
				}
			}
			CoTaskMemFree(id);
			if (conn)
				conn->Release();
			if (topo)
				topo->Release();
			d->Release();
		}
		list->Release();
	}
	en->Release();

	if (found && name)
	{
		IPropertyStore* props = nullptr;
		if (SUCCEEDED(found->OpenPropertyStore(STGM_READ, &props)))
		{
			PROPVARIANT v;
			PropVariantInit(&v);
			if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
				*name = v.pwszVal;
			PropVariantClear(&v);
			props->Release();
		}
	}
	return found;
}

bool SaveWav(const std::wstring& path, const std::vector<int16_t>& samples, DWORD rate)
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"wb") || !f)
		return false;
	const DWORD dataBytes = (DWORD)(samples.size() * sizeof(int16_t));
	WAVEFORMATEX wf{};
	wf.wFormatTag = WAVE_FORMAT_PCM;
	wf.nChannels = 1;
	wf.nSamplesPerSec = rate;
	wf.wBitsPerSample = 16;
	wf.nBlockAlign = 2;
	wf.nAvgBytesPerSec = rate * 2;
	const DWORD fmtBytes = 16, riffBytes = 4 + (8 + fmtBytes) + (8 + dataBytes);
	fwrite("RIFF", 1, 4, f);
	fwrite(&riffBytes, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmtBytes, 4, 1, f);
	fwrite(&wf, fmtBytes, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&dataBytes, 4, 1, f);
	fwrite(samples.data(), 1, dataBytes, f);
	fclose(f);
	return true;
}

double ToDb(double v) { return v > 1e-6 ? 20.0 * log10(v) : -120.0; }

// Records 3 s from the PS3 Eye's mic array, shows a level per mic, and plays the recording back.
int MicTest()
{
	Title(L"Microphone test (3 seconds, then playback)");
	switch (usbdriver::Mic())
	{
	case usbdriver::MicState::NotPlugged:
		Bad(L"No PS3 Eye found.");
		DriverHint(ps3eye::Status::NotFound);
		return 1;
	case usbdriver::MicState::WrongDriver:
		Bad(L"The microphone has the wrong driver. Choose Install (1) to fix it.");
		return 1;
	case usbdriver::MicState::NotStarted:
		Bad(L"The microphone failed to start. The auto-fix task restarts it within seconds; try again, or choose Install (1).");
		return 1;
	case usbdriver::MicState::Ok:
		break;
	}

	std::wstring name;
	IMMDevice* device = FindMicEndpoint(&name);
	if (!device)
	{
		Bad(L"Windows hasn't listed the microphone as a recording device yet. Wait a few seconds and try again.");
		return 1;
	}

	IAudioClient* client = nullptr;
	IAudioCaptureClient* capture = nullptr;
	WAVEFORMATEX* wf = nullptr;
	HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
	if (SUCCEEDED(hr))
		hr = client->GetMixFormat(&wf);
	if (SUCCEEDED(hr))
		hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, wf, nullptr);
	if (SUCCEEDED(hr))
		hr = client->GetService(IID_PPV_ARGS(&capture));
	if (FAILED(hr))
	{
		Bad(L"Can't open \"" + name + L"\": " + HrText(hr));
		if (hr == E_ACCESSDENIED)
			Info(L"Check Settings > Privacy > Microphone: allow desktop apps to access your microphone.");
		if (capture) capture->Release();
		if (client) client->Release();
		CoTaskMemFree(wf);
		device->Release();
		return 1;
	}

	// shared-mode mix format: normally 32-bit float, sometimes 16-bit PCM
	const WAVEFORMATEXTENSIBLE* wfx = (const WAVEFORMATEXTENSIBLE*)wf;
	const bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
		(wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
	const int channels = wf->nChannels;
	const DWORD rate = wf->nSamplesPerSec;

	Info(L"\"" + name + L"\": " + std::to_wstring(channels) + L" mics, " + std::to_wstring(rate) + L" Hz");
	Color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
	wprintf(L"  Recording now: say something!\n");
	Color(7);

	std::vector<double> peak(channels, 0.0), sumSquares(channels, 0.0);
	std::vector<float> mono;
	mono.reserve(rate * 4);
	client->Start();
	const ULONGLONG start = GetTickCount64();
	int shown = -1;
	while (GetTickCount64() - start < 3000)
	{
		int left = 3 - (int)((GetTickCount64() - start) / 1000);
		if (left != shown)
		{
			wprintf(L"  %d...\r", left);
			shown = left;
		}
		Sleep(20);
		UINT32 packet = 0;
		while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet)
		{
			BYTE* data = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;
			if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
				break;
			for (UINT32 i = 0; i < frames; i++)
			{
				float mix = 0;
				for (int c = 0; c < channels; c++)
				{
					float s = 0;
					if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
						s = isFloat ? ((const float*)data)[i * channels + c] : ((const int16_t*)data)[i * channels + c] / 32768.0f;
					peak[c] = std::max(peak[c], (double)std::fabs(s));
					sumSquares[c] += (double)s * s;
					mix += s;
				}
				mono.push_back(mix / channels);
			}
			capture->ReleaseBuffer(frames);
		}
	}
	client->Stop();
	capture->Release();
	client->Release();
	CoTaskMemFree(wf);
	device->Release();
	wprintf(L"         \r");

	if (mono.empty())
	{
		Bad(L"No sound data arrived from the microphone.");
		return 1;
	}

	// a level bar per mic: -60 dB (empty) to 0 dB (full), drawn from the peak
	double loudest = -120;
	for (int c = 0; c < channels; c++)
	{
		const double peakDb = ToDb(peak[c]), avgDb = ToDb(sqrt(sumSquares[c] / mono.size()));
		loudest = std::max(loudest, peakDb);
		const int width = 30, filled = std::clamp((int)((peakDb + 60) / 60 * width + 0.5), 0, width);
		std::wstring bar(filled, L'#');
		bar.append(width - filled, L'.');
		wprintf(L"  Mic %d  [%s]  peak %4.0f dB, average %4.0f dB\n", c + 1, bar.c_str(), peakDb, avgDb);
	}
	for (int c = 0; c < channels; c++)
	{
		if (loudest - ToDb(peak[c]) > 15)
			Info(L"Mic " + std::to_wstring(c + 1) + L" is much quieter than the others (blocked, or a faulty capsule?)");
	}
	if (loudest < -50)
		Bad(L"Almost silent. Is the microphone muted in Windows' sound settings?");
	else
		Ok(L"The microphone hears you");

	// playback: mono, boosted (the PS3 Eye records quietly) by up to 20 dB, peaks kept below clipping
	float monoPeak = 0;
	for (float s : mono)
		monoPeak = std::max(monoPeak, std::fabs(s));
	const double gain = monoPeak > 0 ? std::min(10.0, 0.7 / monoPeak) : 1.0;
	std::vector<int16_t> pcm(mono.size());
	for (size_t i = 0; i < mono.size(); i++)
		pcm[i] = (int16_t)std::clamp(mono[i] * gain * 32767.0, -32768.0, 32767.0);

	const std::wstring wav = TestFolder() + L"\\ps3eye_mic_test.wav";
	if (!SaveWav(wav, pcm, rate))
	{
		Bad(L"Couldn't save the recording to " + wav);
		return 1;
	}
	wchar_t line[160];
	if (gain > 1.1)
		swprintf_s(line, L"Playing it back on your speakers (made %.0f dB louder)...", ToDb(gain));
	else
		swprintf_s(line, L"Playing it back on your speakers...");
	Info(line);
	if (!PlaySoundW(wav.c_str(), nullptr, SND_FILENAME | SND_SYNC | SND_NODEFAULT))
		Bad(L"Playback failed (no speakers or headphones?). The recording is in " + wav);
	else
		Ok(L"Recording saved: " + wav);
	ShowInFolder(wav);
	return 0;
}

void ShowSettings()
{
	Title(L"Settings (HKLM\\SOFTWARE\\PS3EyeVCam) - changes apply live, even mid-call");
	size_t n = 0;
	const vcam::ConfigItem* items = vcam::ConfigItems(&n);
	vcam::Config def;
	ps3eye::Settings d = def.camera;
	const int defaults[] = { d.autoGain, d.autoWhiteBalance, d.gain, d.exposure, d.brightness, d.contrast, d.sharpness, d.hue,
		d.redBalance, d.greenBalance, d.blueBalance, d.flipH, d.flipV, 0 };
	for (size_t i = 0; i < n; i++)
	{
		bool present = false;
		DWORD v = vcam::ReadConfigValue(items[i].name, (DWORD)defaults[i], &present);
		wprintf(L"  %-17s %4lu %s [%d-%d] %s\n", items[i].name, v, present ? L"*" : L" ", items[i].minValue, items[i].maxValue, items[i].help);
	}
	wprintf(L"  (* = changed from default)\n");
}

int Set(const std::wstring& name, const std::wstring& value)
{
	size_t n = 0;
	const vcam::ConfigItem* items = vcam::ConfigItems(&n);
	for (size_t i = 0; i < n; i++)
	{
		if (_wcsicmp(items[i].name, name.c_str()) != 0)
			continue;
		wchar_t* end = nullptr;
		long v = wcstol(value.c_str(), &end, 10);
		if (!end || *end || v < items[i].minValue || v > items[i].maxValue)
		{
			Bad(std::wstring(items[i].name) + L" must be between " + std::to_wstring(items[i].minValue) + L" and " + std::to_wstring(items[i].maxValue));
			return 1;
		}
		LSTATUS st = vcam::WriteConfigValue(items[i].name, (DWORD)v);
		if (st != ERROR_SUCCESS)
		{
			Bad(L"Could not save: " + HrText(HRESULT_FROM_WIN32(st)));
			return 1;
		}
		Ok(std::wstring(items[i].name) + L" = " + std::to_wstring(v));
		return 0;
	}
	Bad(L"Unknown setting '" + name + L"'");
	return 1;
}

int Reset()
{
	size_t n = 0;
	const vcam::ConfigItem* items = vcam::ConfigItems(&n);
	for (size_t i = 0; i < n; i++)
		vcam::DeleteConfigValue(items[i].name);
	Ok(L"All settings back to defaults");
	return 0;
}

void OpenLog()
{
	std::wstring p = vcam::LogPath();
	if (!FileExists(p))
	{
		Info(L"No log yet (it appears once an app opens the camera): " + p);
		return;
	}
	ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

std::wstring Prompt(const wchar_t* text)
{
	wprintf(L"%s", text);
	std::wstring s;
	std::getline(std::wcin, s);
	return s;
}

void SettingsMenu()
{
	for (;;)
	{
		ShowSettings();
		std::wstring line = Prompt(L"\nType 'Name Value' (e.g. FlipHorizontal 1), 'reset', or Enter to go back: ");
		if (line.empty())
			return;
		if (_wcsicmp(line.c_str(), L"reset") == 0)
		{
			Reset();
			continue;
		}
		size_t sp = line.find(L' ');
		if (sp == std::wstring::npos)
			Bad(L"Format: Name Value");
		else
			Set(line.substr(0, sp), line.substr(sp + 1));
	}
}

int Menu()
{
	SetConsoleTitleW(L"PS3 Eye Camera setup");
	for (;;)
	{
		system("cls");
		Title(L"PS3 Eye Camera");
		Status();
		wprintf(L"\n  1  Install / repair\n  2  Test the camera\n  3  Image settings\n  4  Open log\n  5  Uninstall\n  6  Test the microphone\n  0  Exit\n");
		std::wstring c = Prompt(L"\nChoose: ");
		if (c == L"1") Install();
		else if (c == L"2") Test();
		else if (c == L"3") { SettingsMenu(); continue; }
		else if (c == L"4") OpenLog();
		else if (c == L"5") Uninstall();
		else if (c == L"6") MicTest();
		else if (c == L"0" || c.empty() || std::wcin.eof()) return 0;
		Prompt(L"\nPress Enter to continue...");
	}
}

void Usage()
{
	wprintf(L"PS3EyeSetup [install | uninstall | status | test | mictest | settings | set <Name> <Value> | reset | log]\n"
		L"No argument opens the menu.\n");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	MFStartup(MF_VERSION);

	int rc = 0;
	if (argc < 2)
	{
		rc = Menu();
	}
	else
	{
		std::wstring cmd = argv[1];
		for (auto& ch : cmd)
			ch = (wchar_t)towlower(ch);
		if (cmd == L"install") rc = Install();
		else if (cmd == L"uninstall") rc = Uninstall();
		else if (cmd == L"status") rc = Status() ? 1 : 0;
		else if (cmd == L"test") rc = Test();
		else if (cmd == L"settings") ShowSettings();
		else if (cmd == L"set" && argc == 4) rc = Set(argv[2], argv[3]);
		else if (cmd == L"reset") rc = Reset();
		else if (cmd == L"log") OpenLog();
		else if (cmd == L"mictest") rc = MicTest();
		else if (cmd == L"micwatch") rc = MicWatch(); // run by the scheduled task
		else { Usage(); rc = 2; }
	}

	MFShutdown();
	CoUninitialize();
	return rc;
}
