#define MyAppName "GuVi"
#define MyAppVersion "1.0.2"
#define MyAppExe "guvi.exe"

[Setup]
AppId={{A7E4C1B2-6F0D-4E58-9C31-0B5D8E2A71F4}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=Gubke
DefaultDirName={autopf}\GuVi
DefaultGroupName=GuVi
DisableProgramGroupPage=yes
OutputDir=.
OutputBaseFilename=GuVi-Setup
SetupIconFile=..\logo3.ico
UninstallDisplayIcon={app}\{#MyAppExe}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern

[Files]
Source: "..\release\portable\GuVi\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\GuVi"; Filename: "{app}\{#MyAppExe}"
Name: "{autodesktop}\GuVi"; Filename: "{app}\{#MyAppExe}"

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "Launch GuVi"; Flags: nowait postinstall skipifsilent
