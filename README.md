# PS3 Eye Camera for Windows 11

Makes the PlayStation 3 Eye show up as a normal webcam called **"PS3 Eye Camera"** in Zoom, Discord, Teams, OBS, browsers and the Windows Camera app.

No kernel driver, no test-signing, no paid software. It's a user-mode USB driver (libusb + WinUSB) plugged into Windows 11's built-in virtual camera system.

## Install (2 minutes)

1. Plug in the PS3 Eye.
2. Download **`PS3EyeCamera-Setup-x.y.z.exe`** from [Releases](../../releases/latest) and run it.
   - If Windows shows "Windows protected your PC": click **More info → Run anyway** (the installer isn't code-signed).
3. Click through the wizard. It installs everything, including the USB driver (Windows' own WinUSB). No Zadig needed.
4. Pick **PS3 Eye Camera** as the camera in your app.

Afterwards, **PS3 Eye Camera Settings** in the Start menu lets you test the camera, change the image and repair the install. Uninstall from *Settings → Apps*.

No admin rights to install things? The portable zip works too: unzip, run `PS3EyeSetup.exe`, press `1`.

Quick check: open **PS3 Eye Camera Settings** and choose `2` (Test). It grabs 3 seconds of video, shows the fps and opens a snapshot.

## Image settings

Menu option `3`, or from a command prompt:

```
PS3EyeSetup set FlipHorizontal 1
PS3EyeSetup set AutoGain 0
PS3EyeSetup set Exposure 200
PS3EyeSetup reset
```

Changes apply live, even during a call. Defaults: auto gain/exposure and auto white balance on.

## Formats offered

640×480 at 30/60 fps and 320×240 at 30/60/120/187 fps, in NV12, YUY2 and RGB32. Apps pick what they want.

## If something's wrong

| Problem | Fix |
|---|---|
| "USB driver install failed" | Device Manager → *USB Camera-B4.09.24.1* → Update driver → Browse my computer → Let me pick → *Universal Serial Bus devices* → *WinUsb Device*. Then run Install again. |
| Camera listed, but shows a grey "crossed-out camera" picture | The PS3 Eye isn't reachable: check the cable/port, then look at the log (menu `4`). |
| Low fps / frozen picture | Plug straight into the PC, not a hub. Try another USB port. |
| "PS3 Eye Camera" not in the list | Run Install again. Windows 11 only (build 22000+). |
| Test says "in use" | An app is using the camera. Close it, then test. |

Log file: `C:\ProgramData\PS3EyeVCam\vcam.log`

## Uninstall

*Settings → Apps → PS3 Eye Camera → Uninstall* (or menu option `5` in the portable version). It removes the camera, the settings and the USB driver, leaving the PS3 Eye as Windows found it.

## Build it yourself

**GitHub:** every push builds the installer (*Actions* tab → latest run → artifacts).

**Publishing a new version:** change the version at the top of `build.bat` and `publish.bat`, run `build.bat`, then `publish.bat`.

**Local:** double-click `build.bat`. It installs the free C++ build tools and Inno Setup if needed, then builds the installer and portable zip into `dist\`. Change the version at the top of `build.bat`.

## How it works

```
PS3 Eye ──USB/WinUSB──► libusb (src/core) ──Bayer frames──► demosaic → NV12/YUY2/RGB32
                                                                    │
Zoom / Discord / Teams ◄── Windows Camera Frame Server ◄── media source DLL (src/source)
```

- `src/core` — portable PS3 Eye driver: sensor setup, USB streaming, frame assembly, hot-unplug recovery, color conversion. Unit-tested (`tests/`), also builds on Linux.
- `src/source` — Media Foundation media source the Windows Frame Server loads; one capture thread answers sample requests.
- `src/setup` — installer, tester, settings and uninstaller.

## Known limits

- One app or many at once: fine (Windows shares the stream). But while a call is using it, `Test` can't open the camera.
- The PS3 Eye's 4-mic array is not touched: Windows' own USB audio driver keeps handling it.
- Max resolution is the sensor's 640×480.

## License

GPL-2.0-or-later (sensor tables and packet parser come from the Linux `gspca_ov534` driver via [inspirit/PS3EYEDriver](https://github.com/inspirit/PS3EYEDriver)). The Media Foundation scaffolding is adapted from [smourier/VCamSample](https://github.com/smourier/VCamSample) (MIT, see `src/source/LICENSE-VCamSample.txt`). Links [libusb](https://libusb.info) (LGPL-2.1).
