#include "Config.h"

#include <shlobj.h>
#include <algorithm>
#include <iterator>
#include <cstdarg>
#include <cwchar>
#include <mutex>

#pragma comment(lib, "shell32")
#pragma comment(lib, "ole32")
#pragma comment(lib, "advapi32")

namespace vcam {

namespace {

const ConfigItem kItems[] = {
	{ L"AutoGain", 0, 1, L"1 = automatic gain and exposure (recommended), 0 = use Gain/Exposure" },
	{ L"AutoWhiteBalance", 0, 1, L"1 = automatic white balance (recommended), 0 = use Red/Green/BlueBalance" },
	{ L"Gain", 0, 63, L"sensor gain when AutoGain=0" },
	{ L"Exposure", 0, 255, L"exposure when AutoGain=0" },
	{ L"Brightness", 0, 255, L"" },
	{ L"Contrast", 0, 255, L"" },
	{ L"Sharpness", 0, 63, L"" },
	{ L"Hue", 0, 255, L"" },
	{ L"RedBalance", 0, 255, L"when AutoWhiteBalance=0" },
	{ L"GreenBalance", 0, 255, L"when AutoWhiteBalance=0" },
	{ L"BlueBalance", 0, 255, L"when AutoWhiteBalance=0" },
	{ L"FlipHorizontal", 0, 1, L"mirror the image left/right" },
	{ L"FlipVertical", 0, 1, L"turn the image upside down" },
	{ L"DeviceIndex", 0, 7, L"which PS3 Eye to use if several are plugged in" },
};

template <typename T> T Clamped(const wchar_t* name, T fallback)
{
	size_t n = 0;
	const ConfigItem* items = ConfigItems(&n);
	DWORD v = ReadConfigValue(name, (DWORD)fallback);
	for (size_t i = 0; i < n; i++)
	{
		if (!wcscmp(items[i].name, name))
			v = (DWORD)std::clamp<long long>(v, items[i].minValue, items[i].maxValue);
	}
	return (T)v;
}

} // namespace

const ConfigItem* ConfigItems(size_t* count)
{
	*count = std::size(kItems);
	return kItems;
}

DWORD ReadConfigValue(const wchar_t* name, DWORD fallback, bool* present)
{
	DWORD value = 0, size = sizeof(value), type = 0;
	LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, kRegKey, name, RRF_RT_REG_DWORD, &type, &value, &size);
	if (present)
		*present = st == ERROR_SUCCESS;
	return st == ERROR_SUCCESS ? value : fallback;
}

LSTATUS WriteConfigValue(const wchar_t* name, DWORD value)
{
	HKEY key = nullptr;
	LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
	if (st != ERROR_SUCCESS)
		return st;
	st = RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
	RegCloseKey(key);
	return st;
}

LSTATUS DeleteConfigValue(const wchar_t* name)
{
	HKEY key = nullptr;
	LSTATUS st = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegKey, 0, KEY_SET_VALUE, &key);
	if (st != ERROR_SUCCESS)
		return st == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : st;
	st = RegDeleteValueW(key, name);
	RegCloseKey(key);
	return st == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : st;
}

Config LoadConfig()
{
	Config c;
	ps3eye::Settings& s = c.camera;
	s.autoGain = Clamped<int>(L"AutoGain", s.autoGain) != 0;
	s.autoWhiteBalance = Clamped<int>(L"AutoWhiteBalance", s.autoWhiteBalance) != 0;
	s.gain = Clamped<uint8_t>(L"Gain", s.gain);
	s.exposure = Clamped<uint8_t>(L"Exposure", s.exposure);
	s.brightness = Clamped<uint8_t>(L"Brightness", s.brightness);
	s.contrast = Clamped<uint8_t>(L"Contrast", s.contrast);
	s.sharpness = Clamped<uint8_t>(L"Sharpness", s.sharpness);
	s.hue = Clamped<uint8_t>(L"Hue", s.hue);
	s.redBalance = Clamped<uint8_t>(L"RedBalance", s.redBalance);
	s.greenBalance = Clamped<uint8_t>(L"GreenBalance", s.greenBalance);
	s.blueBalance = Clamped<uint8_t>(L"BlueBalance", s.blueBalance);
	s.flipH = Clamped<int>(L"FlipHorizontal", s.flipH) != 0;
	s.flipV = Clamped<int>(L"FlipVertical", s.flipV) != 0;
	c.deviceIndex = Clamped<int>(L"DeviceIndex", 0);
	return c;
}

std::wstring DataFolder()
{
	std::wstring path = L"C:\\ProgramData";
	PWSTR known = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &known)) && known)
		path = known;
	CoTaskMemFree(known);
	return path + L"\\PS3EyeVCam";
}

std::wstring LogPath()
{
	return DataFolder() + L"\\vcam.log";
}

void Log(const wchar_t* format, ...)
{
	static std::mutex mutex;
	std::lock_guard<std::mutex> lock(mutex);

	wchar_t message[1024];
	va_list args;
	va_start(args, format);
	_vsnwprintf_s(message, _TRUNCATE, format, args);
	va_end(args);

	wchar_t exe[MAX_PATH] = L"";
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	const wchar_t* exeName = wcsrchr(exe, L'\\') ? wcsrchr(exe, L'\\') + 1 : exe;

	SYSTEMTIME t;
	GetLocalTime(&t);
	wchar_t line[1400];
	swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu %s] %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
		GetCurrentProcessId(), exeName, message);

	char utf8[4096];
	int len = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
	if (len <= 1)
		return;

	std::wstring path = LogPath();
	WIN32_FILE_ATTRIBUTE_DATA info{};
	if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && (info.nFileSizeHigh || info.nFileSizeLow > 1024 * 1024))
		MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);

	HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;
	DWORD written = 0;
	WriteFile(file, utf8, (DWORD)(len - 1), &written, nullptr);
	CloseHandle(file);
}

} // namespace vcam
