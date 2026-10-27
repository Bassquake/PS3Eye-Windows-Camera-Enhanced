#include "SharedFrames.h"

#include "../core/rates.h"

#include <sddl.h>
#include <algorithm>
#include <cstring>
#include <string>

#pragma comment(lib, "advapi32")

namespace vcam {

namespace {

// ".2": the layout changed (frame sizes), so a server and filter from different versions never meet
const wchar_t* kMappingName = L"Local\\PS3EyeVCam.Frames.2";
const wchar_t* kServerMutexName = L"Local\\PS3EyeVCam.Server.2";
const wchar_t* kFrameEventName = L"Local\\PS3EyeVCam.Frame.2.";

constexpr size_t kHeaderBytes = (sizeof(SharedHeader) + 4095) & ~size_t(4095);
constexpr size_t kMappingBytes = kHeaderBytes + 2 * (size_t)kSharedFrameBytes;

// Everyone in this session, including low-integrity processes (some apps capture from a
// sandboxed helper). The objects live in the session's Local\ namespace.
SECURITY_ATTRIBUTES* OpenSecurity()
{
	static SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, FALSE };
	static bool made = false;
	if (!made)
	{
		made = true;
		ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);
	}
	return sa.lpSecurityDescriptor ? &sa : nullptr;
}

std::wstring EventName(int slot)
{
	return kFrameEventName + std::to_wstring(slot);
}

} // namespace

SharedFrames::~SharedFrames()
{
	Close();
}

bool SharedFrames::Open()
{
	if (_header)
		return true;
	_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, OpenSecurity(), PAGE_READWRITE, 0, (DWORD)kMappingBytes, kMappingName);
	if (!_mapping)
		return false;
	_header = (SharedHeader*)MapViewOfFile(_mapping, FILE_MAP_ALL_ACCESS, 0, 0, kMappingBytes);
	if (!_header)
	{
		CloseHandle(_mapping);
		_mapping = nullptr;
		return false;
	}
	return true;
}

void SharedFrames::Close()
{
	Release();
	for (HANDLE& e : _slotEvents)
	{
		if (e)
			CloseHandle(e);
		e = nullptr;
	}
	if (_header)
		UnmapViewOfFile(_header);
	if (_mapping)
		CloseHandle(_mapping);
	_header = nullptr;
	_mapping = nullptr;
}

uint8_t* SharedFrames::Buffer(uint64_t seq) const
{
	return (uint8_t*)_header + kHeaderBytes + (size_t)(seq % 2) * kSharedFrameBytes;
}

// ---- reader side ---------------------------------------------------------------

int SharedFrames::Claim(uint32_t fps, uint32_t width)
{
	if (!_header)
		return -1;
	if (_slot >= 0)
	{
		InterlockedExchange(&_header->slots[_slot].fps, (LONG)fps);
		InterlockedExchange(&_header->slots[_slot].width, (LONG)width);
		Refresh();
		return _slot;
	}

	const ULONGLONG now = GetTickCount64();
	for (int i = 0; i < kSharedSlots; i++)
	{
		SharedSlot& s = _header->slots[i];
		LONG64 hb = s.heartbeat;
		if (hb != 0 && now - (ULONGLONG)hb < kSlotTimeoutMs)
			continue;
		if (InterlockedCompareExchange64(&s.heartbeat, (LONG64)now, hb) != hb)
			continue;
		InterlockedExchange(&s.fps, (LONG)fps);
		InterlockedExchange(&s.width, (LONG)width);
		InterlockedExchange(&s.pid, (LONG)GetCurrentProcessId());
		_slot = i;
		_event = CreateEventW(OpenSecurity(), FALSE, FALSE, EventName(i).c_str());
		return i;
	}
	return -1;
}

void SharedFrames::Refresh()
{
	if (_header && _slot >= 0)
		InterlockedExchange64(&_header->slots[_slot].heartbeat, (LONG64)GetTickCount64());
}

void SharedFrames::Release()
{
	if (_header && _slot >= 0)
	{
		SharedSlot& s = _header->slots[_slot];
		InterlockedExchange(&s.fps, 0);
		InterlockedExchange(&s.width, 0);
		InterlockedExchange(&s.pid, 0);
		InterlockedExchange64(&s.heartbeat, 0);
	}
	_slot = -1;
	if (_event)
		CloseHandle(_event);
	_event = nullptr;
}

bool SharedFrames::ReadLatest(uint64_t& lastSeq, uint8_t* dst, int& width, int& height, bool* signal)
{
	if (!_header)
		return false;
	for (int attempt = 0; attempt < 3; attempt++)
	{
		const LONG64 seq = InterlockedCompareExchange64(&_header->sequence, 0, 0);
		if (seq <= 0 || (uint64_t)seq == lastSeq)
			return false;
		const bool real = _header->signal != 0;
		const LONG w = _header->width[seq % 2], h = _header->height[seq % 2];
		if (w <= 0 || h <= 0 || (size_t)w * h * 4 > kSharedFrameBytes)
			return false;
		memcpy(dst, Buffer((uint64_t)seq), (size_t)w * h * 4);
		MemoryBarrier();
		// the writer only touches this buffer again after publishing the next frame
		if (InterlockedCompareExchange64(&_header->sequence, 0, 0) == seq)
		{
			lastSeq = (uint64_t)seq;
			width = (int)w;
			height = (int)h;
			if (signal)
				*signal = real;
			return true;
		}
	}
	return false;
}

// ---- writer side -----------------------------------------------------------------

uint8_t* SharedFrames::WriteBuffer()
{
	return _header ? Buffer((uint64_t)_header->sequence + 1) : nullptr;
}

HANDLE SharedFrames::OpenSlotEvent(int slot) const
{
	return OpenEventW(EVENT_MODIFY_STATE, FALSE, EventName(slot).c_str());
}

void SharedFrames::Publish(bool signal, uint32_t width, uint32_t height)
{
	if (!_header)
		return;
	const LONG64 next = _header->sequence + 1;
	InterlockedExchange(&_header->width[next % 2], (LONG)width);
	InterlockedExchange(&_header->height[next % 2], (LONG)height);
	InterlockedExchange(&_header->signal, signal ? 1 : 0);
	InterlockedIncrement64(&_header->sequence);

	const ULONGLONG now = GetTickCount64();
	for (int i = 0; i < kSharedSlots; i++)
	{
		LONG64 hb = _header->slots[i].heartbeat;
		if (hb == 0 || now - (ULONGLONG)hb >= kSlotTimeoutMs)
			continue;
		if (!_slotEvents[i])
			_slotEvents[i] = OpenSlotEvent(i);
		if (_slotEvents[i])
			SetEvent(_slotEvents[i]);
	}
}

WantedMode SharedFrames::Wanted(ULONGLONG now) const
{
	WantedMode m{ kSharedWidth, kSharedHeight, 0, 0 };
	bool allSmall = true;
	if (_header)
	{
		for (const SharedSlot& s : _header->slots)
		{
			LONG64 hb = s.heartbeat;
			if (hb == 0 || now - (ULONGLONG)hb >= kSlotTimeoutMs)
				continue;
			m.readers++;
			const LONG want = s.fps;
			m.fps = std::max<uint32_t>(m.fps, want > 0 ? (uint32_t)want : 1);
			allSmall &= s.width > 0 && s.width <= 320;
		}
	}
	// 640x480 halved looks better than the sensor's own 320x240, so that's only used for the
	// rates 640x480 can't reach
	if (m.readers > 0 && allSmall && m.fps > ps3eye::MaxFps(kSharedWidth))
	{
		m.width = 320;
		m.height = 240;
	}
	m.fps = std::min(m.fps, ps3eye::MaxFps(m.width));
	return m;
}

// ---- server process ----------------------------------------------------------------

HANDLE CreateServerMutex()
{
	HANDLE m = CreateMutexW(OpenSecurity(), TRUE, kServerMutexName);
	if (m && GetLastError() == ERROR_ALREADY_EXISTS)
	{
		CloseHandle(m);
		return nullptr;
	}
	return m;
}

bool SharedFrames::ServerRunning()
{
	HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, kServerMutexName);
	if (!m)
		return GetLastError() == ERROR_ACCESS_DENIED; // exists, just not ours to open
	CloseHandle(m);
	return true;
}

bool SharedFrames::StartServer(const wchar_t* dir)
{
	std::wstring exe = std::wstring(dir) + L"\\" + kServerExe;
	std::wstring cmd = L"\"" + exe + L"\"";
	STARTUPINFOW si{ sizeof(si) };
	PROCESS_INFORMATION pi{};
	// break away from the app's job object so closing that app doesn't kill the server
	// other apps are still reading from
	BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB | CREATE_DEFAULT_ERROR_MODE,
		nullptr, dir, &si, &pi);
	if (!ok)
		ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_DEFAULT_ERROR_MODE, nullptr, dir, &si, &pi);
	if (!ok)
		return false;
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

} // namespace vcam
