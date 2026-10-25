// Settings (HKLM\SOFTWARE\PS3EyeVCam) and the log file shared by the camera DLL and the setup tool.
#pragma once

#include <windows.h>
#include <string>
#include "../core/ps3eye_camera.h"

namespace vcam {

constexpr const wchar_t* kRegKey = L"SOFTWARE\\PS3EyeVCam";
constexpr const wchar_t* kCameraName = L"PS3 Eye Camera";

struct Config
{
	ps3eye::Settings camera;
	int deviceIndex = 0;
};

struct ConfigItem
{
	const wchar_t* name;
	int minValue;
	int maxValue;
	const wchar_t* help;
};

// All tunable values, in the order the setup tool lists them.
const ConfigItem* ConfigItems(size_t* count);

Config LoadConfig();
DWORD ReadConfigValue(const wchar_t* name, DWORD fallback, bool* present = nullptr);
LSTATUS WriteConfigValue(const wchar_t* name, DWORD value);   // needs admin
LSTATUS DeleteConfigValue(const wchar_t* name);               // needs admin

std::wstring DataFolder();   // C:\ProgramData\PS3EyeVCam
std::wstring LogPath();      // C:\ProgramData\PS3EyeVCam\vcam.log
void Log(const wchar_t* format, ...);

} // namespace vcam
