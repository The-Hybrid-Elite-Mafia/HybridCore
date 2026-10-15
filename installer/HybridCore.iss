; =============================================================================
;  The Hybrid Core — Windows installer (Inno Setup 6)
;  Built by CI:  ISCC /DAppVersion=x.y.z /DBinDir=<build tools dir> /DAsioLicense=<path> HybridCore.iss
;  AppId is permanent: never change it, or upgrades will install side-by-side.
; =============================================================================
#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef BinDir
  #define BinDir "..\build\rig-asio"
#endif

[Setup]
AppId={{464ED1CE-0D55-42BF-A38A-AD0EF6AB480F}
AppName=The Hybrid Core
AppVersion={#AppVersion}
AppVerName=The Hybrid Core {#AppVersion}
AppPublisher=Hybrid Elite Mafia
DefaultDirName={autopf}\Hybrid Elite Mafia\The Hybrid Core
DefaultGroupName=The Hybrid Core
DisableProgramGroupPage=yes
OutputBaseFilename=HybridCore-Setup-{#AppVersion}
OutputDir=output
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
PrivilegesRequired=admin
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=The Hybrid Core
CloseApplications=yes

[Dirs]
; Settings live here and survive upgrades AND uninstall.
Name: "{userappdata}\HybridCore"; Flags: uninsneveruninstall

[Files]
Source: "{#BinDir}\tools\device_probe\Release\hc_device_probe.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#BinDir}\tools\engine_smoke\Release\hc_engine_smoke.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#BinDir}\tests\Release\hc_tests.exe";                     DestDir: "{app}\bin"; Flags: ignoreversion
Source: "HOW-TO-TEST.txt";                                          DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "THIRD-PARTY-NOTICES.txt";                                  DestDir: "{app}"; Flags: ignoreversion
#ifdef AsioLicense
Source: "{#AsioLicense}"; DestDir: "{app}\licenses"; DestName: "Steinberg-ASIO-SDK-LICENSE.txt"; Flags: ignoreversion
#endif

[Icons]
; cmd /k keeps the window open so the results can be read.
Name: "{group}\1 - Rig Check";               Filename: "{cmd}"; Parameters: "/k """"{app}\bin\hc_device_probe.exe"" --seconds 2"""; WorkingDir: "{app}\bin"
Name: "{group}\2 - Mixer Test (WASAPI)";     Filename: "{cmd}"; Parameters: "/k """"{app}\bin\hc_engine_smoke.exe"" --mixer --seconds 15"""; WorkingDir: "{app}\bin"
Name: "{group}\3 - Mixer Test (ASIO)";       Filename: "{cmd}"; Parameters: "/k """"{app}\bin\hc_engine_smoke.exe"" --asio --mixer --seconds 15"""; WorkingDir: "{app}\bin"
Name: "{group}\4 - Engine Self-Test";        Filename: "{cmd}"; Parameters: "/k """"{app}\bin\hc_tests.exe"""""; WorkingDir: "{app}\bin"
Name: "{group}\How to test";                 Filename: "{app}\HOW-TO-TEST.txt"
Name: "{group}\Uninstall The Hybrid Core";   Filename: "{uninstallexe}"
Name: "{autodesktop}\Hybrid Core - Rig Check"; Filename: "{cmd}"; Parameters: "/k """"{app}\bin\hc_device_probe.exe"" --seconds 2"""; WorkingDir: "{app}\bin"
