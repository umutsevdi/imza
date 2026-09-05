; ------------------------------------------------------------------------------
; File: i18n/build.sh
; Created: 08/22/25
; Author: Umut Sevdi
; Description: Generates the Windows Installer.
;
; Project: umutsevdi/imcircuit
; License: 
; GNU GENERAL PUBLIC LICENSE
; ------------------------------------------------------------------------------

; Change the following variable to path to the source code.
#define Source "C:\Users\vboxuser\source\repos\ursa"
#define Build Source + "\build\x86-Release"

#define ursaName "Ursa"
#define ursaDescription "Open source multi-modal coding agent."
#define ursaVersion "0.1.1"
#define ursaAuthor "Umut Sevdi"
#define ursaURL "https://github.com/umutsevdi/ursa"
#define ursaExe "ursa.exe"
#define ursaAssoc ursaName + " File"
#define ursaCopyright "Copyright (C) 2026 Umut Sevdi"
[Setup]
AppId={{F8F8F40F-329B-4F1D-86A0-BC654325E25E}
AppName={#ursaName}
AppVersion={#ursaVersion}
AppVerName={#ursaName} - {#ursaVersion}
AppPublisher={#ursaAuthor}
AppPublisherURL={#ursaURL}
AppSupportURL={#ursaURL}
AppContact={#ursaAuthor}
AppComments={#ursaDescription}
AppCopyright={#ursaCopyright}
AppUpdatesURL={#ursaURL}

DefaultDirName={autopf}\{#ursaName}
DisableDirPage=yes

ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

ChangesEnvironment=yes
DisableProgramGroupPage=yes
LicenseFile={#Source}\LICENSE
PrivilegesRequired=lowest

OutputDir={#Build}
OutputBaseFilename=Ursa Installer
SetupIconFile={#Build}\package\win\ursa.ico
UninstallDisplayIcon={#Build}\package\win\ursa.ico
UninstallDisplayName={#ursaName}

SolidCompression=yes
WizardStyle=modern dynamic

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "addtopath"; Description: "Add Ursa to the PATH environment variable"

[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Flags: preservestringtype; Tasks: addtopath

[Files]
Source: "{#Build}\release\{#ursaExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\release\libcurl.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\release\z.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\package\win\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#ursaName}"; Filename: "{app}\{#ursaExe}"
Name: "{autodesktop}\{#ursaName}"; Filename: "{app}\{#ursaExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#ursaExe}"; Description: "{cm:LaunchProgram,{#StringChange(ursaName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
