// PS3EyeServer.exe (Windows 10 path): owns the PS3 Eye and shares its frames with the DirectShow
// filter in every app that has "PS3 Eye Camera" open. The filter starts it on demand; it exits a
// few seconds after the last app stops reading. No window, one instance per session.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include "../core/convert.h"
#include "../core/ps3eye_camera.h"
#include "../source/Config.h"
#include "SharedFrames.h"

namespace {

using namespace std::chrono;
using Clock = steady_clock;

constexpr auto kIdleExit = seconds(5);      // no readers for this long -> exit
constexpr auto kStartupGrace = seconds(10); // the app that launched us may take a while to claim a slot

int Run(vcam::SharedFrames& shared)
{
	ps3eye::Camera camera;
	ps3eye::Status lastStatus = ps3eye::Status::Ok; // only log a failure when it changes
	vcam::Config config = vcam::LoadConfig();
	ps3eye::ImageAdjust adjust = vcam::ImageAdjustFor(config.camera);
	std::vector<uint8_t> bayer;

	const auto started = Clock::now();
	auto lastReader = started;
	auto lastAttempt = Clock::now() - seconds(10);
	auto lastFrame = Clock::now();
	auto lastPlaceholder = Clock::now() - seconds(10);
	uint32_t width = vcam::kSharedWidth, height = vcam::kSharedHeight, fps = 0;
	uint64_t frames = 0;
	auto statsStart = Clock::now();

	// live settings: re-read the registry key whenever it changes
	HKEY key = nullptr;
	HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	auto watchSettings = [&]
	{
		if (!key)
			RegOpenKeyExW(HKEY_LOCAL_MACHINE, vcam::kRegKey, 0, KEY_NOTIFY | KEY_READ, &key);
		if (key && changed)
			RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC, changed, TRUE);
	};
	watchSettings();

	for (;;)
	{
		const vcam::WantedMode wanted = shared.Wanted(GetTickCount64());
		const int readers = wanted.readers;
		if (readers > 0)
		{
			lastReader = Clock::now();
		}
		else
		{
			if (camera.IsStreaming())
			{
				vcam::Log(L"No apps reading; camera off");
				camera.Stop();
			}
			lastAttempt = Clock::now() - seconds(10); // start at once when an app comes back
			if (Clock::now() - lastReader >= kIdleExit && Clock::now() - started >= kStartupGrace)
				break;
			std::this_thread::sleep_for(milliseconds(100));
			continue;
		}

		// an app asking for a faster rate (or 320x240 beyond 640x480's limit) restarts the sensor
		const uint32_t rate = ps3eye::Camera::SupportedFps(wanted.width, std::max<uint32_t>(wanted.fps, 1));
		if (rate != fps || wanted.width != width)
		{
			if (camera.IsStreaming())
				camera.Stop();
			fps = rate;
			width = wanted.width;
			height = wanted.height;
			lastAttempt = Clock::now() - seconds(10);
		}

		// (re)connect
		if (!camera.IsStreaming() || camera.DeviceLost())
		{
			if (camera.IsStreaming())
			{
				vcam::Log(L"Camera lost (unplugged or USB error); retrying every second");
				camera.Stop();
			}

			if (Clock::now() - lastAttempt >= seconds(1))
			{
				lastAttempt = Clock::now();
				ps3eye::Status st = camera.Start(width, height, fps, config.camera, config.deviceIndex);
				if (st == ps3eye::Status::Ok)
				{
					vcam::Log(L"Camera streaming %ux%u @ %u fps for %d app(s)", camera.Width(), camera.Height(), camera.Fps(), readers);
					InterlockedExchange(&shared.Header()->fps, (LONG)camera.Fps());
					lastFrame = Clock::now();
				}
				else if (st != lastStatus)
				{
					vcam::Log(L"Camera start failed: %S", ps3eye::StatusText(st));
				}
				lastStatus = st;
			}
		}

		// frames
		if (camera.IsStreaming() && !camera.DeviceLost())
		{
			if (camera.WaitFrame(bayer, milliseconds(100)))
			{
				const uint32_t w = camera.Width(), h = camera.Height();
				ps3eye::BayerToBGRX(bayer.data(), (int)w, (int)h, shared.WriteBuffer(), (long)w * 4);
				ps3eye::AdjustBGRX(shared.WriteBuffer(), (int)w, (int)h, (long)w * 4, adjust);
				shared.Publish(true, w, h);
				lastFrame = Clock::now();

				frames++;
				if (Clock::now() - statsStart >= minutes(5))
				{
					double secs = duration<double>(Clock::now() - statsStart).count();
					vcam::Log(L"Camera running at %.1f fps", frames / secs);
					statsStart = Clock::now();
					frames = 0;
				}
			}
			else if (Clock::now() - lastFrame > seconds(3))
			{
				vcam::Log(L"No frames for 3 s; restarting the camera");
				camera.Stop();
			}
		}
		else
		{
			// no camera: keep apps alive with a placeholder at 5 fps
			if (Clock::now() - lastPlaceholder >= milliseconds(200))
			{
				lastPlaceholder = Clock::now();
				ps3eye::DrawNoSignal(shared.WriteBuffer(), (int)vcam::kSharedWidth, (int)vcam::kSharedHeight, (long)vcam::kSharedWidth * 4);
				shared.Publish(false, vcam::kSharedWidth, vcam::kSharedHeight);
			}
			std::this_thread::sleep_for(milliseconds(20));
		}

		// settings changed?
		if (changed && WaitForSingleObject(changed, 0) == WAIT_OBJECT_0)
		{
			vcam::Config next = vcam::LoadConfig();
			vcam::Log(L"Settings changed; applying");
			if (next.deviceIndex != config.deviceIndex)
				camera.Stop(); // reconnect to the other camera
			else
				camera.Apply(next.camera);
			config = next;
			adjust = vcam::ImageAdjustFor(config.camera);
			watchSettings();
		}
	}

	camera.Stop();
	if (key)
		RegCloseKey(key);
	if (changed)
		CloseHandle(changed);
	return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	HANDLE mutex = vcam::CreateServerMutex();
	if (!mutex)
		return 0; // already running in this session

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

	vcam::SharedFrames shared;
	if (!shared.Open())
	{
		vcam::Log(L"Server: can't create the shared frame memory (error %lu)", GetLastError());
		return 1;
	}
	InterlockedExchange(&shared.Header()->serverPid, (LONG)GetCurrentProcessId());
	vcam::Log(L"Server started");

	int rc = Run(shared);

	InterlockedExchange(&shared.Header()->serverPid, 0);
	InterlockedExchange(&shared.Header()->fps, 0);
	vcam::Log(L"Server exiting (no apps using the camera)");
	ReleaseMutex(mutex);
	CloseHandle(mutex);
	return rc;
}
