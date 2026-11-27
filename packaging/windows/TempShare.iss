#ifndef AppVersion
  #define AppVersion "0.5.1"
#endif
#ifndef StagingDir
  #define StagingDir "..\..\build\dist"
#endif
#ifndef OutputPath
  #define OutputPath "..\..\build\package"
#endif

[Setup]
AppId={{2A8AF975-6BD3-4551-BE0A-C8721C39DE2E}
AppName=TempShare
AppVersion={#AppVersion}
AppPublisher=sudoevolve
AppPublisherURL=https://github.com/sudoevolve/TempShare
AppSupportURL=https://github.com/sudoevolve/TempShare/issues
AppUpdatesURL=https://github.com/sudoevolve/TempShare/releases
DefaultDirName={localappdata}\Programs\TempShare
DefaultGroupName=TempShare
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputPath}
OutputBaseFilename=TempShare-{#AppVersion}-windows-x64
LicenseFile=..\..\LICENSE
SetupIconFile=tempshare.ico
UninstallDisplayIcon={app}\bin\tempshare.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Files]
Source: "{#StagingDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\TempShare"; Filename: "{app}\bin\tempshare.exe"; WorkingDir: "{localappdata}\TempShare"; AppUserModelID: "sudoevolve.TempShare"
Name: "{autoprograms}\Uninstall TempShare"; Filename: "{uninstallexe}"

[Dirs]
Name: "{localappdata}\TempShare"; Flags: uninsneveruninstall

[Run]
Filename: "{app}\bin\tempshare.exe"; WorkingDir: "{localappdata}\TempShare"; Description: "TempShare"; Flags: nowait postinstall skipifsilent
