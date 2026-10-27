; Installer wizard for PS3 Eye Camera. Built by build.bat with Inno Setup 6 or 7:
;   ISCC.exe /DAppVersion=1.1.0 /DDistDir=..\dist installer\PS3EyeCamera.iss
;
; The wizard stages the files in {app}\installer and runs "PS3EyeSetup.exe install" from there.
; That tool copies them into {app} (handling a DLL still loaded by the Windows camera service),
; registers the camera and sets up the USB driver. It picks the camera type itself: the Windows 11
; virtual camera (PS3EyeVCam.dll), or on Windows 10 the DirectShow filters (PS3EyeDShow64/32.dll)
; fed by PS3EyeServer.exe.

#ifndef AppVersion
  #define AppVersion "1.1.0"
#endif
#ifndef DistDir
  #define DistDir "..\dist"
#endif

[Setup]
AppId={{C4BF9929-859F-4A33-9EA8-18ED97AF3B79}
AppName=PS3 Eye Camera
AppVersion={#AppVersion}
AppVerName=PS3 Eye Camera {#AppVersion}
AppPublisher=Bassquake
DefaultDirName={autopf}\PS3EyeVCam
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=no
LicenseFile=..\LICENSE
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.10240
OutputDir={#DistDir}
OutputBaseFilename=PS3EyeCamera-Setup-{#AppVersion}
UninstallDisplayName=PS3 Eye Camera
UninstallDisplayIcon={app}\PS3EyeSetup.exe
SetupIconFile=..\src\res\ps3eye.ico
; the camera from icon.png (tools/make_artwork.py makes them): large = Welcome/Finish pages,
; small = page headers (with transparency); 100/150/200% sizes, Setup picks the one that fits
WizardImageFile=wizard\large-100.bmp,wizard\large-150.bmp,wizard\large-200.bmp
WizardSmallImageFile=wizard\small-100.bmp,wizard\small-150.bmp,wizard\small-200.bmp
WizardImageAlphaFormat=defined
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=no

[Messages]
WelcomeLabel2=This will install the PS3 Eye Camera driver, so your PlayStation 3 Eye works as a normal webcam in Zoom, Discord, Teams, OBS and browsers (and the Camera app on Windows 11).%n%nPlug in the PS3 Eye before clicking Next.
WindowsVersionNotSupported=PS3 Eye Camera needs Windows 10 or 11.

[Files]
Source: "{#DistDir}\PS3EyeSetup.exe"; DestDir: "{app}\installer"; Flags: ignoreversion
Source: "{#DistDir}\PS3EyeVCam.dll"; DestDir: "{app}\installer"; Flags: ignoreversion
Source: "{#DistDir}\PS3EyeServer.exe"; DestDir: "{app}\installer"; Flags: ignoreversion
Source: "{#DistDir}\PS3EyeDShow64.dll"; DestDir: "{app}\installer"; Flags: ignoreversion
Source: "{#DistDir}\PS3EyeDShow32.dll"; DestDir: "{app}\installer"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; DestName: "README.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\PS3 Eye Camera Settings"; Filename: "{app}\PS3EyeSetup.exe"; Comment: "Test the camera, change image settings, repair"

[Run]
Filename: "{app}\PS3EyeSetup.exe"; Description: "Open PS3 Eye Camera Settings to test the camera"; Flags: postinstall shellexec nowait skipifsilent unchecked

[UninstallRun]
Filename: "{app}\installer\PS3EyeSetup.exe"; Parameters: "uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveCamera"

[UninstallDelete]
Type: files; Name: "{app}\PS3EyeSetup.exe"
Type: files; Name: "{app}\PS3EyeVCam.dll"
Type: files; Name: "{app}\PS3EyeServer.exe"
Type: files; Name: "{app}\PS3EyeDShow64.dll"
Type: files; Name: "{app}\PS3EyeDShow32.dll"
Type: files; Name: "{app}\*.old*"
Type: dirifempty; Name: "{app}"

[Code]
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep <> ssPostInstall then
    exit;

  WizardForm.StatusLabel.Caption := 'Setting up the camera and its USB driver (a console window shows progress)...';
  if not Exec(ExpandConstant('{app}\installer\PS3EyeSetup.exe'), 'install', ExpandConstant('{app}\installer'),
    SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode) then
  begin
    MsgBox('Could not run the camera setup: ' + SysErrorMessage(ResultCode), mbError, MB_OK);
    exit;
  end;

  case ResultCode of
    0: ;
    2: MsgBox('PS3 Eye Camera is installed, but the camera itself wasn''t ready.' + #13#10#13#10 +
              'Plug the PS3 Eye straight into the PC, then open "PS3 Eye Camera Settings" from the Start menu and choose 1 (Install).',
              mbInformation, MB_OK);
  else
    MsgBox('The camera setup did not finish (error ' + IntToStr(ResultCode) + ').' + #13#10#13#10 +
           'Open "PS3 Eye Camera Settings" from the Start menu and choose 1 (Install) to see what went wrong.',
           mbError, MB_OK);
  end;
end;
