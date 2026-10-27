// Image controls apps can change (DirectShow IAMVideoProcAmp / IAMCameraControl / IAMVideoControl,
// and the matching KS properties on Windows 11). They read and write the same registry settings as
// the setup tool, so a change from any app is saved and applied live by whoever runs the camera.
#pragma once

#include <windows.h>

namespace vcam {

enum class ControlGroup { ProcAmp, Camera };

// The VideoProcAmp_* / CameraControl_* (= KSPROPERTY_VIDEOPROCAMP_* / KSPROPERTY_CAMERACONTROL_*) ids.
struct ControlRange
{
	long min;
	long max;
	long step;
	long def;
	bool canAuto;    // false: manual only
};

// False for a property the PS3 Eye doesn't have.
bool GetControlRange(ControlGroup group, long id, ControlRange& range);
bool GetControl(ControlGroup group, long id, long& value, bool& autoMode);
// Out-of-range values are clamped. Fails with E_ACCESSDENIED when the settings key isn't
// writable (installed by an older version: run Install again).
HRESULT SetControl(ControlGroup group, long id, long value, bool autoMode);

// Mirror/flip as IAMVideoControl / KSPROPERTY_VIDEOCONTROL_MODE flags (1 = horizontal, 2 = vertical).
long GetFlipMode();
HRESULT SetFlipMode(long mode);

} // namespace vcam
