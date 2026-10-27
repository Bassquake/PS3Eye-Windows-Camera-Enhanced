#include "Controls.h"

#include <strmif.h>
#include <algorithm>

#include "Config.h"

namespace vcam {

namespace {

// White balance is a colour temperature for apps; the sensor has red/blue gains. 4650 K is the
// neutral point (both gains 128); warmer light needs more blue, cooler light more red.
constexpr long kWbMin = 2800, kWbMax = 6500, kWbNeutral = 4650;

long KelvinFromRed(int red)
{
	return std::clamp<long>(kWbNeutral + (red - 128) * (kWbNeutral - kWbMin) / 127, kWbMin, kWbMax);
}

int RedFromKelvin(long kelvin)
{
	return std::clamp<int>(128 + (int)((kelvin - kWbNeutral) * 127 / (kWbNeutral - kWbMin)), 0, 255);
}

HRESULT Write(const wchar_t* name, long value)
{
	LSTATUS st = WriteConfigValue(name, (DWORD)value);
	return st == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(st);
}

struct Simple
{
	ControlGroup group;
	long id;
	const wchar_t* name;
	long max;
	int ps3eye::Settings::*field;
	const wchar_t* autoName;   // the shared auto switch, if any
};

// Gain and exposure share the sensor's single auto switch (AGC + AEC).
const Simple kSimple[] = {
	{ ControlGroup::ProcAmp, VideoProcAmp_Brightness, L"Brightness", 255, &ps3eye::Settings::brightness, nullptr },
	{ ControlGroup::ProcAmp, VideoProcAmp_Contrast, L"Contrast", 255, &ps3eye::Settings::contrast, nullptr },
	{ ControlGroup::ProcAmp, VideoProcAmp_Saturation, L"Saturation", 255, &ps3eye::Settings::saturation, nullptr },
	{ ControlGroup::ProcAmp, VideoProcAmp_Hue, L"Hue", 359, &ps3eye::Settings::hue, nullptr },
	{ ControlGroup::ProcAmp, VideoProcAmp_Sharpness, L"Sharpness", 63, &ps3eye::Settings::sharpness, nullptr },
	{ ControlGroup::ProcAmp, VideoProcAmp_Gain, L"Gain", 63, &ps3eye::Settings::gain, L"AutoGain" },
	{ ControlGroup::Camera, CameraControl_Exposure, L"Exposure", 255, &ps3eye::Settings::exposure, L"AutoGain" },
};

const Simple* FindSimple(ControlGroup group, long id)
{
	for (const Simple& s : kSimple)
	{
		if (s.group == group && s.id == id)
			return &s;
	}
	return nullptr;
}

bool IsWhiteBalance(ControlGroup group, long id)
{
	return group == ControlGroup::ProcAmp && id == VideoProcAmp_WhiteBalance;
}

} // namespace

bool GetControlRange(ControlGroup group, long id, ControlRange& range)
{
	const ps3eye::Settings def;
	if (const Simple* s = FindSimple(group, id))
	{
		range = { 0, s->max, 1, def.*s->field, s->autoName != nullptr };
		return true;
	}
	if (IsWhiteBalance(group, id))
	{
		range = { kWbMin, kWbMax, 1, KelvinFromRed(def.redBalance), true };
		return true;
	}
	return false;
}

bool GetControl(ControlGroup group, long id, long& value, bool& autoMode)
{
	const ps3eye::Settings s = LoadConfig().camera;
	if (const Simple* c = FindSimple(group, id))
	{
		value = s.*c->field;
		autoMode = c->autoName && s.autoGain;
		return true;
	}
	if (IsWhiteBalance(group, id))
	{
		value = KelvinFromRed(s.redBalance);
		autoMode = s.autoWhiteBalance;
		return true;
	}
	return false;
}

HRESULT SetControl(ControlGroup group, long id, long value, bool autoMode)
{
	if (const Simple* c = FindSimple(group, id))
	{
		// auto: only flip the switch, so the manual value is kept for later
		if (c->autoName && autoMode)
			return Write(c->autoName, 1);
		HRESULT hr = Write(c->name, std::clamp<long>(value, 0, c->max));
		if (SUCCEEDED(hr) && c->autoName)
			hr = Write(c->autoName, 0);
		return hr;
	}
	if (IsWhiteBalance(group, id))
	{
		if (autoMode)
			return Write(L"AutoWhiteBalance", 1);
		const int red = RedFromKelvin(std::clamp(value, kWbMin, kWbMax));
		HRESULT hr = Write(L"RedBalance", red);
		if (SUCCEEDED(hr))
			hr = Write(L"BlueBalance", 256 - std::max(red, 1));
		if (SUCCEEDED(hr))
			hr = Write(L"AutoWhiteBalance", 0);
		return hr;
	}
	return E_INVALIDARG;
}

long GetFlipMode()
{
	const ps3eye::Settings s = LoadConfig().camera;
	return (s.flipH ? 1 : 0) | (s.flipV ? 2 : 0);
}

HRESULT SetFlipMode(long mode)
{
	HRESULT hr = Write(L"FlipHorizontal", (mode & 1) ? 1 : 0);
	if (SUCCEEDED(hr))
		hr = Write(L"FlipVertical", (mode & 2) ? 1 : 0);
	return hr;
}

} // namespace vcam
