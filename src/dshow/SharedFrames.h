// Frame channel between PS3EyeServer.exe (owns the camera) and the DirectShow filter loaded in
// each app (Windows 10 path). Only one process can claim the PS3 Eye over USB, so the server
// grabs frames once and every app reads them from this shared memory.
//
// Layout: a header with one slot per reader, then two BGRX frame buffers of up to 640x480. The
// server writes frame N+1 into buffer (N+1)%2 while readers copy frame N from the other one, then
// bumps 'sequence'. A reader re-checks 'sequence' after copying to detect a lap.
//
// Readers claim a slot and refresh its heartbeat; the server runs the camera while any slot
// is fresh, at the highest frame rate asked for, and signals each slot's event per frame.
// It runs the sensor at 640x480 unless every reader wants 320x240 faster than 640x480 can go.
#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace vcam {

constexpr uint32_t kSharedWidth = 640;
constexpr uint32_t kSharedHeight = 480;
constexpr uint32_t kSharedFrameBytes = kSharedWidth * kSharedHeight * 4;
constexpr int kSharedSlots = 16;
constexpr ULONGLONG kSlotTimeoutMs = 3000;   // a reader that stops refreshing for this long is gone

constexpr const wchar_t* kServerExe = L"PS3EyeServer.exe";

struct SharedSlot
{
	volatile LONG64 heartbeat;   // GetTickCount64() of the last refresh, 0 = free
	volatile LONG fps;           // rate this reader wants
	volatile LONG pid;
	volatile LONG width;         // frame width this reader delivers (640 or 320)
	volatile LONG reserved;
};

struct SharedHeader
{
	volatile LONG64 sequence;    // frames published so far; frame N lives in buffer N % 2
	volatile LONG signal;        // 1 = the last frame is a real camera picture
	volatile LONG fps;           // rate the camera is running at
	volatile LONG serverPid;
	volatile LONG width[2];      // size of the frame in each buffer
	volatile LONG height[2];
	SharedSlot slots[kSharedSlots];
};

// What the server should run the sensor at for the current readers.
struct WantedMode
{
	uint32_t width;
	uint32_t height;
	uint32_t fps;
	int readers;
};

class SharedFrames
{
public:
	SharedFrames() = default;
	~SharedFrames();
	SharedFrames(const SharedFrames&) = delete;
	SharedFrames& operator=(const SharedFrames&) = delete;

	// Creates or opens the shared memory (either side may come first).
	bool Open();
	void Close();
	bool IsOpen() const { return _header != nullptr; }

	SharedHeader* Header() const { return _header; }

	// ---- reader side (filter) ----
	// Claims a slot for a reader delivering 'width' wide frames; returns its index or -1 when all are taken.
	int Claim(uint32_t fps, uint32_t width);
	void Refresh();        // heartbeat; call at least every second
	void Release();
	HANDLE FrameEvent() const { return _event; }   // signalled per published frame

	// Copies the newest frame into dst (BGRX, room for kSharedFrameBytes, rows width*4 bytes)
	// if it is newer than 'lastSeq', and says its size.
	bool ReadLatest(uint64_t& lastSeq, uint8_t* dst, int& width, int& height, bool* signal);

	// ---- writer side (server) ----
	uint8_t* WriteBuffer();                 // buffer for the next frame
	// makes it current (width x height, rows width*4 bytes) and wakes the readers
	void Publish(bool signal, uint32_t width, uint32_t height);
	WantedMode Wanted(ULONGLONG now) const;

	// Whether PS3EyeServer.exe is running in this session, and launching it from 'dir'.
	static bool ServerRunning();
	static bool StartServer(const wchar_t* dir);

private:
	uint8_t* Buffer(uint64_t seq) const;
	HANDLE OpenSlotEvent(int slot) const;

	HANDLE _mapping = nullptr;
	SharedHeader* _header = nullptr;
	int _slot = -1;
	HANDLE _event = nullptr;
	HANDLE _slotEvents[kSharedSlots] = {};   // writer side, opened lazily
};

// Server only: the "server is running" mutex, or nullptr if another server already holds it.
HANDLE CreateServerMutex();

} // namespace vcam
