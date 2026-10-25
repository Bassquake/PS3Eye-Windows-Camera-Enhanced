// Binds Windows' built-in WinUSB driver to the PS3 Eye's video interface, so no Zadig is needed.
// Same result as Device Manager > Update driver > Browse > Let me pick > Universal Serial Bus devices
// > WinUsb Device, done through SetupAPI. Needs admin.
#pragma once

#include <string>

namespace usbdriver {

enum class Result
{
	AlreadyInstalled,
	Installed,
	NotPlugged,
	Failed,
};

// Driver currently bound to the PS3 Eye's video interface ("WinUSB", "" for none, ...).
std::wstring CurrentService();

// Installs WinUSB on the video interface if needed. 'error' gets a message on failure.
Result Install(std::wstring& error);

// Removes the WinUSB binding (and a Zadig driver package if one was used) and rescans,
// so the camera goes back to how Windows found it.
bool Remove(std::wstring& error);

} // namespace usbdriver
