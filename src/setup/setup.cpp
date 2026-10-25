// PS3EyeSetup.exe: installs, tests, configures and removes the "PS3 Eye Camera" virtual camera.
// Double-click for a menu, or run with a command:
//   install | uninstall | status | test | settings | set <Name> <Value> | reset | log
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfvirtualcamera.h>

#include <chrono>
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

namespace {

const wchar_t* kClsid = L"{C4D641C7-CF26-435F-849B-56F2A715F55E}"; // must match dllmain.cpp
const wchar_t* kDllName = L"PS3EyeVCam.dll";
const wchar_t* kExeName = L"PS3EyeSetup.exe";

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

	if (VirtualCameraApi())
		Ok(L"Windows 11 virtual camera support");
	else
	{
		Bad(L"This Windows version has no virtual camera API (Windows 11 is required)");
		problems++;
	}

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
	}

	Info(L"Log: " + vcam::LogPath());
	return problems;
}

int Install()
{
	Title(L"Installing");
	if (!VirtualCameraApi())
	{
		Bad(L"Windows 11 is required (virtual cameras need build 22000 or newer).");
		return 1;
	}

	const std::wstring srcDir = ExeDir(), dstDir = InstallDir();
	const std::wstring srcDll = srcDir + L"\\" + kDllName, dstDll = dstDir + L"\\" + kDllName;
	if (!FileExists(srcDll))
	{
		Bad(std::wstring(L"Can't find ") + kDllName + L" next to this program. Keep both files in the same folder.");
		return 1;
	}

	// 1. files -> Program Files (the camera service can't read files inside your user folder)
	CreateDirectoryW(dstDir.c_str(), nullptr);
	if (_wcsicmp(srcDir.c_str(), dstDir.c_str()) != 0)
	{
		RemoveVirtualCamera(); // release the old DLL before replacing it
		if (!InstallFile(srcDll, dstDll) || !InstallFile(srcDir + L"\\" + kExeName, dstDir + L"\\" + kExeName))
		{
			Bad(L"Could not copy files to " + dstDir + L": " + HrText(HRESULT_FROM_WIN32(GetLastError())));
			return 1;
		}
	}
	Ok(L"Files copied to " + dstDir);

	// 2. log folder writable by the camera service (LOCAL SERVICE) and users
	std::wstring data = vcam::DataFolder();
	CreateDirectoryW(data.c_str(), nullptr);
	RunHidden(L"icacls.exe \"" + data + L"\" /grant *S-1-5-19:(OI)(CI)M *S-1-5-32-545:(OI)(CI)M /Q");
	Ok(L"Log folder ready: " + data);

	// settings key must exist so the camera can watch it for live changes
	HKEY settingsKey = nullptr;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, vcam::kRegKey, 0, nullptr, 0, KEY_READ, nullptr, &settingsKey, nullptr) == ERROR_SUCCESS)
		RegCloseKey(settingsKey);

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
	HRESULT hr = RemoveVirtualCamera();
	if (SUCCEEDED(hr))
		Ok(L"Virtual camera removed");
	else
		Info(L"Virtual camera removal: " + HrText(hr));

	std::wstring dll = RegisteredDllPath();
	if (dll.empty())
		dll = InstallDir() + L"\\" + kDllName;
	if (FileExists(dll))
		CallDllExport(dll, "DllUnregisterServer");
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"Software\\Classes\\CLSID\\") + kClsid).c_str());
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, vcam::kRegKey);
	Ok(L"Driver unregistered and settings removed");

	const std::wstring dir = InstallDir();
	RemoveFileNowOrLater(dir + L"\\" + kDllName);
	RemoveFileNowOrLater(dir + L"\\" + kExeName);
	if (!RemoveDirectoryW(dir.c_str()))
		MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
	Ok(L"Files removed (anything in use goes at next reboot)");

	std::wstring error;
	if (usbdriver::Remove(error))
		Ok(L"USB driver removed (camera is back to how Windows found it)");
	else
		Info(L"USB driver removal: " + error);

	if (VirtualCameraVisible())
		Info(L"Still listed? Device Manager > Cameras > PS3 Eye Camera > Uninstall device.");
	return 0;
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
	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	std::wstring snap = std::wstring(tmp) + L"ps3eye_snapshot.bmp";
	if (SaveBmp(snap, bgrx, 640, 480))
	{
		Ok(L"Snapshot saved and opened: " + snap);
		ShellExecuteW(nullptr, L"open", snap.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
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
		Title(L"PS3 Eye Camera for Windows 11");
		Status();
		wprintf(L"\n  1  Install / repair\n  2  Test the camera\n  3  Image settings\n  4  Open log\n  5  Uninstall\n  0  Exit\n");
		std::wstring c = Prompt(L"\nChoose: ");
		if (c == L"1") Install();
		else if (c == L"2") Test();
		else if (c == L"3") { SettingsMenu(); continue; }
		else if (c == L"4") OpenLog();
		else if (c == L"5") Uninstall();
		else if (c == L"0" || c.empty() || std::wcin.eof()) return 0;
		Prompt(L"\nPress Enter to continue...");
	}
}

void Usage()
{
	wprintf(L"PS3EyeSetup [install | uninstall | status | test | settings | set <Name> <Value> | reset | log]\n"
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
		else { Usage(); rc = 2; }
	}

	MFShutdown();
	CoUninitialize();
	return rc;
}
