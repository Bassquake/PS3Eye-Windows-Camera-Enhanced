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

// ---- microphone array (USB audio class, interface 1) ----
// Windows' own USB audio driver ("usbaudio") handles it; nothing of ours is installed there.

enum class MicState
{
	Ok,           // usbaudio bound
	NotPlugged,
	WrongDriver,  // something else bound, e.g. a Zadig "USB Serial (CDC)" leftover
	NotStarted,   // usbaudio bound but failed to start (Device Manager code 10)
};

// 'service' gets the bound driver ("usbaudio", "usbser", "" for none).
MicState Mic(std::wstring* service = nullptr);

// ---- Device Manager names ----
// The camera calls itself "USB Camera-B4.09.24.1". These set friendly names on its video and
// microphone interfaces instead ("PS3 Eye Camera", "PS3 Eye Microphone"), or with restore=true
// remove them again. Only writes when a name differs, so it's cheap to call often (the mic
// watcher does). Needs admin. Names belong to the device node, which Windows keys by USB port
// (the PS3 Eye has no serial number), and a node that RepairMic had to re-create starts without
// one, so call this again after a repair.
bool SetDeviceNames(bool restore, std::wstring& error);
bool DeviceNamesSet();

// Removes a wrong driver from the microphone interface (and its oemNN.inf package, so it
// doesn't come back) and lets Windows bind its built-in USB audio driver; a mic whose audio
// driver failed to start is re-detected the same way. Needs admin.
// Returns AlreadyInstalled when the mic was fine, Installed when fixed.
Result RepairMic(std::wstring& error);

} // namespace usbdriver
