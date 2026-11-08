; Inno Setup script for the native Farewell Edition.
; Built in CI: iscc /DAppVersion=5.0.0 /DSourceDir=<deployed folder> /O<out> installer.iss
; Per-user install by default (no administrator rights needed), optional
; all-users install when elevated.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist\exFAT Image Builder"
#endif

[Setup]
AppId={{7C1E3D52-7A36-4C6B-9E1F-5F2E8A0B6D41}
AppName=exFAT Image Builder - Farewell Edition
AppVersion={#AppVersion}
AppVerName=exFAT Image Builder - Farewell Edition {#AppVersion}
AppPublisher=kerrdec97
AppPublisherURL=https://github.com/kerrdec97/ps5-exfat-builder
DefaultDirName={autopf}\exFAT Image Builder Farewell
DefaultGroupName=exFAT Image Builder
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=exFAT-Image-Builder-Farewell-{#AppVersion}-win64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\exFAT Image Builder.exe
LicenseFile={#SourceDir}\LICENCE.txt
MinVersion=10.0.17763

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\exFAT Image Builder - Farewell Edition"; Filename: "{app}\exFAT Image Builder.exe"
Name: "{autodesktop}\exFAT Image Builder - Farewell Edition"; Filename: "{app}\exFAT Image Builder.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Run]
Filename: "{app}\exFAT Image Builder.exe"; Description: "Start exFAT Image Builder"; Flags: nowait postinstall skipifsilent
