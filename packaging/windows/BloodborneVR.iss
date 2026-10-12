; Inno Setup 6 script of the Bloodborne VR one-click installer. Built by tools/release/make_release.py:
;   ISCC.exe /DMyVersion=0.1.0-alpha5 /DStageDir=<out>\BloodborneVR-<ver> /DOutDir=<out> /DRepoDir=<repo> BloodborneVR.iss
; The installer is per user by default (no admin rights, %LOCALAPPDATA%\Programs\BloodborneVR). The player's own files
; (saves, settings, logs, crash reports, shader caches, extracted VR data) live in %LOCALAPPDATA%\BloodborneVR, outside
; the program folder (installed.flag next to BloodborneVR.exe tells the launcher), so upgrades and the uninstaller
; never touch them. The installer carries no game file; it does carry NVIDIA's nvngx_dlss.dll + bbport_dlss.dll (in the stage's bin/,
; with the NVIDIA licence text in licenses/; see tools/release/make_release.py).
#ifndef MyVersion
  #define MyVersion "0.0.0"
#endif
#ifndef MyNumVersion
  #define MyNumVersion "0.0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\..\out\release\BloodborneVR-" + MyVersion
#endif
#ifndef OutDir
  #define OutDir "..\..\out\release"
#endif
#ifndef RepoDir
  #define RepoDir "..\.."
#endif

[Setup]
; Never change AppId: it is how a new Setup.exe finds (and upgrades in place) an older install.
AppId={{6D3B0C58-7A41-4E0B-9C0F-B10D0D8E5A21}
AppName=Bloodborne VR
AppVersion={#MyVersion}
AppVerName=Bloodborne VR {#MyVersion}
AppPublisher=Bloodborne VR contributors
AppPublisherURL=https://github.com/laughtt/bloodborne-vr
AppSupportURL=https://github.com/laughtt/bloodborne-vr/issues
AppUpdatesURL=https://github.com/laughtt/bloodborne-vr/releases
VersionInfoVersion={#MyNumVersion}
DefaultDirName={localappdata}\Programs\BloodborneVR
DefaultGroupName=Bloodborne VR
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile={#StageDir}\COPYING.GPL-3.0
InfoBeforeFile={#RepoDir}\packaging\windows\installer-notes.txt
InfoAfterFile={#RepoDir}\packaging\windows\installer-smartscreen.txt
SetupIconFile={#RepoDir}\launcher\bloodborne.ico
UninstallDisplayIcon={app}\BloodborneVR.exe
UninstallDisplayName=Bloodborne VR
OutputDir={#OutDir}
OutputBaseFilename=BloodborneVR-{#MyVersion}-Setup
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
UsePreviousAppDir=yes
UsePreviousTasks=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
FinishedLabel=Setup has installed [name] on your computer.%n%nWindows SmartScreen may warn about "BloodborneVR.exe" the first time (the alpha is not code-signed): click "More info", then "Run anyway".%n%nStart "Bloodborne VR" from the Start menu, choose your game folder and press PLAY.

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; Tells the launcher this is an installed copy: user data goes to %LOCALAPPDATA%\BloodborneVR.
Source: "{#RepoDir}\packaging\windows\installed.flag"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Bloodborne VR"; Filename: "{app}\BloodborneVR.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\Bloodborne VR crash reports"; Filename: "{localappdata}\BloodborneVR\crashes"
Name: "{autodesktop}\Bloodborne VR"; Filename: "{app}\BloodborneVR.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\BloodborneVR.exe"; Description: "Launch Bloodborne VR"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Only what running the program wrote inside the program folder. Saves, settings, logs, crash reports and shader
; caches are in %LOCALAPPDATA%\BloodborneVR and stay.
Type: filesandordirs; Name: "{app}\launcher\__pycache__"
Type: filesandordirs; Name: "{app}\scripts\__pycache__"
Type: filesandordirs; Name: "{app}\scripts\vr_assets\__pycache__"
Type: filesandordirs; Name: "{app}\python\__pycache__"
Type: filesandordirs; Name: "{app}\out"
Type: filesandordirs; Name: "{app}\logs"
Type: dirifempty; Name: "{app}"

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    if not UninstallSilent then
      MsgBox('Bloodborne VR was removed. Your saves, settings, logs and crash reports were kept in' + #13#10 +
             ExpandConstant('{localappdata}\BloodborneVR') + #13#10 + 'Delete that folder by hand if you want them gone.',
             mbInformation, MB_OK);
end;
