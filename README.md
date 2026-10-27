# PS3 Eye Windows Camera Enhanced

This is a fork of [ps3eye-windows-camera](https://github.com/aasmh/ps3eye-windows-camera) from aasmh, so credit goes to him for kickstarting this one. Its a no kernel driver, no test-signing, no paid piece of software, and it's a user-mode USB driver (libusb + WinUSB) plugged into Windows 11's built-in virtual camera system, or on Windows 10 into DirectShow, the way OBS Virtual Camera works. 

## Enhancements

- Makes the PlayStation 3 Eye show up on Windows 10 and 11 as a normal webcam called **"PS3 Eye Camera"** in Zoom, Discord, Teams, OBS, browsers and (on Windows 11) the Windows Camera app.
- Built for ARM64, x86 and x64.
- Can adjust the camera properties such as Brightness and Contrast. All available frame rates should be visible to apps.
- PS3 Microphone can be used too!

![Screenshot of OBS using the PS3 Eye Camera](https://github.com/Bassquake/ps3eye-windows-camera-enhanced/blob/main/captures/obs-properties-cam.png)

## Install (2 minutes)

1. Plug in the PS3 Eye.
2. Download **`PS3EyeCamera-Setup-x.y.z.exe`** from [Releases](../../releases/latest) and run it.
   - If Windows shows "Windows protected your PC": click **More info → Run anyway** (the installer isn't code-signed).
3. Click through the wizard. It installs everything, including the USB driver (Windows' own WinUSB). No Zadig needed.
4. Pick **PS3 Eye Camera** as the camera in your app.

Afterwards, **PS3 Eye Camera Settings** in the Start menu lets you test the camera, change the image and repair the install. Uninstall from *Settings → Apps*.

No admin rights to install things? The portable zip works too: unzip, run `PS3EyeSetup.exe`, press `1`.

**Windows on ARM** (Snapdragon laptops, Surface Pro X and so on): the same installer works and installs the native ARM64 version by itself. For the portable version, use the `arm64-portable` zip. The ARM64 version is built from the same code but hasn't been tried on ARM hardware yet, so reports are welcome.

**32-bit Windows 10**: the same installer works here too and installs the 32-bit (x86) version by itself; the portable one is the `x86-portable` zip. It hasn't been tried on 32-bit Windows yet either.

**Linux**: you don't need this project. Linux supports the PS3 Eye out of the box (the kernel's `gspca_ov534` driver): plug it in and it's a normal webcam (`/dev/video0`) in every app, and the microphone array works as a USB sound card. Change brightness, exposure, gain and so on with `v4l2-ctl`, `guvcview` or OBS. The kernel driver goes up to 60 fps at 640x480 and 125 fps at 320x240.

Quick check: open **PS3 Eye Camera Settings** and choose `2` (Test). It grabs 3 seconds of video, shows the fps and saves a snapshot. From a command prompt you can test other modes too, e.g. `PS3EyeSetup test 640 75` or `PS3EyeSetup test 320 187`.

Microphone check: choose `6` (Test the microphone). It records 3 seconds, shows a level bar for each of the 4 mics, and plays the recording back on your speakers (boosted, since the PS3 Eye records quietly). Same from a command prompt: `PS3EyeSetup mictest`.

Both tests save their result in *Documents\PS3 Eye Camera Tests* (`ps3eye_snapshot.bmp`, `ps3eye_mic_test.wav`), replacing the previous one, and open that folder when they finish.

## Image settings

Apps can change them from their own camera settings (OBS: *Properties → Configure Video*; anything else that offers brightness, contrast and so on): brightness, contrast, saturation, hue, sharpness, gain and exposure (auto or manual), white balance (auto, or a colour temperature) and mirror/flip. Or use menu option `3`, or a command prompt:

```
PS3EyeSetup set FlipHorizontal 1
PS3EyeSetup set AutoGain 0
PS3EyeSetup set Exposure 200
PS3EyeSetup set Saturation 0
PS3EyeSetup reset
```

Either way it's one shared set of settings: it's saved, every app sees the same values, and changes apply live, even during a call. Defaults: auto gain/exposure and auto white balance on, everything else neutral.

Gain, exposure, white balance and flips are set on the camera's sensor. Brightness, contrast, saturation, hue and sharpness are applied on the PC, because the sensor only does them in a mode the PS3 Eye can't stream fast enough over USB; they cost some CPU while they're away from their defaults.

## Formats offered

640×480 at up to 75 fps and 320×240 at up to 187 fps (every rate the sensor has: 2–75 and 2–187), in NV12, YUY2 and RGB32 on Windows 11, plus I420 and RGB24 on Windows 10. Apps pick what they want.

## If something's wrong

| Problem | Fix |
|---|---|
| "USB driver install failed" | Device Manager → *USB Camera-B4.09.24.1* → Update driver → Browse my computer → Let me pick → *Universal Serial Bus devices* → *WinUsb Device*. Then run Install again. |
| Camera listed, but shows a grey "crossed-out camera" picture | The PS3 Eye isn't reachable: check the cable/port, then look at the log (menu `4`). |
| Low fps / frozen picture | Plug straight into the PC, not a hub. Try another USB port. |
| PS3 Eye microphone missing, or the camera shows up as a COM port | Run Install again: it replaces a leftover Zadig driver on the microphone with Windows' USB audio driver, and sets the mic up again if it failed to start (error 10 in Device Manager; the PS3 Eye's mic sometimes fails to start when plugged in or after a restart). Status (menu) shows whether the mic is OK. |
| "PS3 Eye Camera" not in the list | Run Install again. On Windows 10 the built-in Camera app (and other Store apps) can't see it: use Zoom, Teams, Discord, OBS or a browser. |
| Test says "in use" | An app is using the camera. Close it, then test. |

Log file: `C:\ProgramData\PS3EyeVCam\vcam.log`

## Uninstall

*Settings → Apps → PS3 Eye Camera → Uninstall* (or menu option `5` in the portable version). It removes the camera, the settings and the USB driver, leaving the PS3 Eye as Windows found it.

## Build it yourself

Double-click `build.bat`. It installs the free C++ build tools (including the ARM64 compiler) and Inno Setup if needed, then builds x64, ARM64 and x86, the installer (all three in one) and the portable zips (one each) into `dist\`. The loose files for each go in `dist\x64\`, `dist\arm64\` and `dist\x86\`. Change the version at the top of `build.bat`.

The program icon and the installer wizard pictures are made from `icon.png`. After changing it, run `python tools/make_artwork.py` (needs Python with Pillow: `pip install pillow`), then `build.bat`.

## How it works

Windows 11:

```
PS3 Eye ──USB/WinUSB──► libusb (src/core) ──Bayer frames──► demosaic → NV12/YUY2/RGB32
                                                                    │
Zoom / Discord / Teams ◄── Windows Camera Frame Server ◄── media source DLL (src/source)
```

Windows 10 (no virtual camera API):

```
PS3 Eye ──USB/WinUSB──► PS3EyeServer.exe ──shared memory (BGRX frames)──► DirectShow filter in each app
                       (libusb + demosaic)                                  (PS3EyeDShow64/32.dll)
                                                                                   │
                                          Zoom / Teams / Discord / OBS / browsers ◄┘ YUY2/NV12/I420/RGB24/RGB32
```

- `src/core` — portable PS3 Eye driver: sensor setup, USB streaming, frame assembly, hot-unplug recovery, color conversion. Unit-tested (`tests/`), also builds on Linux.
- `src/source` — Media Foundation media source the Windows Frame Server loads; one capture thread answers sample requests.
- `src/dshow` — Windows 10: DirectShow capture filter (64- and 32-bit, so 32-bit apps see it too) and `PS3EyeServer.exe`, which owns the camera and shares frames with every app using it. The filter starts the server on demand; it quits a few seconds after the last app stops.
- `src/setup` — installer, tester, settings and uninstaller.

## Known limits

- One app or many at once: fine (Windows shares the stream). But while a call is using it, `Test` can't open the camera.
- The PS3 Eye's 4-mic array (4 channels, 16 kHz) is handled by Windows' own USB audio driver and shows up as *Microphone (PS3 Eye Microphone)*. If an old Zadig install put a different driver on it (often "USB Serial (CDC)", making it appear as a COM port), Install puts Windows' audio driver back. The mic's USB audio part sometimes fails to start (at random: on plug-in, after a restart or a driver reinstall). Install retries until it starts, and a small scheduled task (*PS3 Eye Camera microphone fix*, runs `PS3EyeSetup micwatch` as SYSTEM from startup) does the same automatically within seconds of a plug-in or within 15 s otherwise. Uninstall removes it.
- Device Manager lists the camera as *PS3 Eye Camera* and its mic as *PS3 Eye Microphone* instead of *USB Camera-B4.09.24.1*. Install sets the names, and the microphone task re-applies them (on another USB port, or after a mic repair), so apps list the mic as *Microphone (PS3 Eye Microphone)*.
- Max resolution is the sensor's 640×480.
- Windows 10: all apps share one camera stream, so it runs at the fastest rate any of them asks for. 320×240 above 75 fps needs the sensor in its 320×240 mode, so it only happens while every app using the camera wants 320×240; otherwise those apps get up to 75 fps. Store apps such as the Camera app and Windows Hello only list real webcams there, so they won't see it.

## License

GPL-2.0-or-later (sensor tables and packet parser come from the Linux `gspca_ov534` driver via [inspirit/PS3EYEDriver](https://github.com/inspirit/PS3EYEDriver)). The Media Foundation scaffolding is adapted from [smourier/VCamSample](https://github.com/smourier/VCamSample) (MIT, see `src/source/LICENSE-VCamSample.txt`). Links [libusb](https://libusb.info) (LGPL-2.1).
