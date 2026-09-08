; ------------------------------------------------------------------------------
; File: win.iss
; Created: 05/09/26
; Author: Umut Sevdi
; Description: Generates the Windows Installer.
;
; Project: umutsevdi/imza
; License: 
; GNU GENERAL PUBLIC LICENSE
; ------------------------------------------------------------------------------

; Change the following variable to path to the source code.
#define Source "C:\Users\vboxuser\source\repos\imza"
#define Build Source + "\build\x64-Release"

#define imzaName "Imza"
#define imzaDescription "Open source multi-modal coding agent."
#define imzaVersion "0.2.0"
#define imzaAuthor "Umut Sevdi"
#define imzaURL "https://github.com/umutsevdi/imza"
#define imzaExe "imza.exe"
#define imzaAssoc imzaName + " File"
#define imzaCopyright "Copyright (C) 2026 Umut Sevdi"
[Setup]
AppId={{AC922EA9-C1E0-4FA1-8529-8E9D701DF81C}
AppName={#imzaName}
AppVersion={#imzaVersion}
AppVerName={#imzaName} - {#imzaVersion}
AppPublisher={#imzaAuthor}
AppPublisherURL={#imzaURL}
AppSupportURL={#imzaURL}
AppContact={#imzaAuthor}
AppComments={#imzaDescription}
AppCopyright={#imzaCopyright}
AppUpdatesURL={#imzaURL}

DefaultDirName={autopf}\{#imzaName}
DisableDirPage=yes

ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

ChangesEnvironment=yes
DisableProgramGroupPage=yes
LicenseFile={#Source}\LICENSE
PrivilegesRequired=lowest

OutputDir={#Build}
OutputBaseFilename=Imza Installer
SetupIconFile={#Build}\package\win\imza.ico
UninstallDisplayIcon={#Build}\package\win\imza.ico
UninstallDisplayName={#imzaName}

SolidCompression=yes
WizardStyle=modern dynamic

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "addtopath"; Description: "Add Imza to the PATH environment variable"

[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Flags: preservestringtype; Tasks: addtopath

[Files]
Source: "{#Build}\release\{#imzaExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Source}\misc\LICENSE.thirdparty.txt"; DestDir: "{app}"
Source: "{#Source}\misc\imza.ico"; DestDir: "{app}"
Source: "{#Source}\LICENSE"; DestDir: "{app}"
Source: "{#Source}\CHANGELOG.txt"; DestDir: "{app}"
Source: "{#Source}\README.md"; DestDir: "{app}"

[Icons]
Name: "{autoprograms}\{#imzaName}"; Filename: "{app}\{#imzaExe}"
Name: "{autodesktop}\{#imzaName}"; Filename: "{app}\{#imzaExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#imzaExe}"; Description: "{cm:LaunchProgram,{#StringChange(imzaName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
