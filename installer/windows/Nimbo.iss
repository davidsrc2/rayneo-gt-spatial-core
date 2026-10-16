; Nimbo for Windows: one installer with the app, the Virtual Display Driver and the Visual C++
; runtime. Built by scripts/windows/make_installer.ps1 (Inno Setup 6), which passes:
;   AppDir     published app (dotnet publish: Nimbo.exe, nimbo_server.dll, nimbo_win.exe, ONNX DLLs)
;   DriverDir  Virtual Display Driver files (MttVDD.inf / .cat / .dll, vdd_settings.xml)
;   Redist     vc_redist.x64.exe
;   Version, OutputDir

#ifndef Version
  #define Version "0.1.0"
#endif

[Setup]
AppId={{6E2B7A51-9C3D-4F0E-8B6A-2D1C5E7F9A30}
AppName=Nimbo
AppVersion={#Version}
AppPublisher=gestorae.es
AppPublisherURL=https://github.com/davidsrc2/rayneo-gt-spatial-core
DefaultDirName={autopf}\Nimbo
DefaultGroupName=Nimbo
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=Nimbo-Setup-{#Version}
SetupIconFile=..\..\assets\icon\Nimbo.ico
UninstallDisplayIcon={app}\Nimbo.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; The driver and its on/off tasks need administrator rights.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
CloseApplications=yes

[Languages]
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
es.DesktopIcon=Crear un acceso directo en el escritorio
en.DesktopIcon=Create a desktop shortcut
es.StartWithWindows=Abrir Nimbo al iniciar Windows
en.StartWithWindows=Open Nimbo when Windows starts
es.Extras=Opciones:
en.Extras=Options:
es.InstallingRuntime=Instalando Visual C++ Runtime…
en.InstallingRuntime=Installing the Visual C++ Runtime…
es.InstallingDriver=Instalando y preparando las pantallas virtuales…
en.InstallingDriver=Installing and preparing the virtual displays…
es.RemovingDriver=Quitando las pantallas virtuales…
en.RemovingDriver=Removing the virtual displays…
es.Launch=Abrir Nimbo
en.Launch=Open Nimbo

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopIcon}"; GroupDescription: "{cm:Extras}"
Name: "startup"; Description: "{cm:StartWithWindows}"; GroupDescription: "{cm:Extras}"; Flags: unchecked

[Files]
Source: "{#AppDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#DriverDir}\*"; DestDir: "{app}\driver"; Flags: ignoreversion
Source: "{#Redist}"; DestDir: "{tmp}"; DestName: "vc_redist.x64.exe"; Flags: deleteafterinstall; Check: NeedsVCRedist

[Icons]
Name: "{autoprograms}\Nimbo"; Filename: "{app}\Nimbo.exe"
Name: "{autodesktop}\Nimbo"; Filename: "{app}\Nimbo.exe"; Tasks: desktopicon

[Registry]
; The same value the app's "Abrir Nimbo al iniciar Windows" switch writes (Core/Startup.cs).
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Nimbo"; \
  ValueData: """{app}\Nimbo.exe"" --background"; Flags: uninsdeletevalue; Tasks: startup

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "{cm:InstallingRuntime}"; \
  Flags: waituntilterminated; Check: NeedsVCRedist
; Installs the shipped driver if there is none, a pool of 6 monitors, switched on only while
; Nimbo runs (scheduled tasks), no restart.
Filename: "{app}\nimbo_win.exe"; Parameters: "--setup-displays"; StatusMsg: "{cm:InstallingDriver}"; \
  Flags: runhidden waituntilterminated
Filename: "{app}\Nimbo.exe"; Description: "{cm:Launch}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{app}\nimbo_win.exe"; Parameters: "--restore"; Flags: runhidden waituntilterminated; RunOnceId: "RestoreDisplays"
Filename: "{app}\nimbo_win.exe"; Parameters: "--uninstall-driver"; StatusMsg: "{cm:RemovingDriver}"; \
  Flags: runhidden waituntilterminated; RunOnceId: "RemoveDriver"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\driver"

[Code]
// Visual C++ 2015-2022 runtime (x64) already there: skip its installer.
function NeedsVCRedist: Boolean;
var
  Installed: Cardinal;
begin
  Result := not (RegQueryDWordValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed)
                 and (Installed = 1));
end;

// Nimbo running would hold its files and the displays: closed first.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM Nimbo.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM nimbo_win.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Result := '';
end;

function InitializeUninstall: Boolean;
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM Nimbo.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM nimbo_win.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Result := True;
end;
