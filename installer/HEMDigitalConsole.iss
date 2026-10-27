; =============================================================================
;  HEM Digital Console — Windows installer (Inno Setup 6)
;  Built by CI:  ISCC /DAppVersion=x.y.z /DBinDir=<build dir> /DAsioLicense=<SDK LICENSE.txt> HEMDigitalConsole.iss
;  Licence: GPLv3 or later. Sound: ASIO (Steinberg's ASIO SDK under its GPLv3 option) or WASAPI.
;  AppId is permanent: never change it, or upgrades will install side-by-side.
; =============================================================================
#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef BinDir
  #define BinDir "..\build\rig-asio"
#endif

[Setup]
AppId={{FFD1E588-17FF-4CE8-92A8-49B5109CD4BB}
AppName=HEM Digital Console
AppVersion={#AppVersion}
AppVerName=HEM Digital Console {#AppVersion}
AppPublisher=Hybrid Elite Mafia
DefaultDirName={autopf}\Hybrid Elite Mafia\HEM Digital Console
DefaultGroupName=HEM Digital Console
DisableProgramGroupPage=yes
OutputBaseFilename=HEMDigitalConsole-Setup-{#AppVersion}
OutputDir=output
SetupIconFile=..\console\app\resources\HEMDigitalConsole.ico
UninstallDisplayIcon={app}\HEMDigitalConsole.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
PrivilegesRequired=admin
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=HEM Digital Console
CloseApplications=yes
ChangesAssociations=yes

; Settings and the last session live in %APPDATA%\HEM Digital Console (settings.ini, autosave.hemc).
; This installer never creates, overwrites or deletes that folder, so they survive upgrades and uninstall.

[Files]
Source: "{#BinDir}\console\app\Release\HEMDigitalConsole.exe";      DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\console\app\Release\hem_console_snapshot.exe";   DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#BinDir}\tests\Release\hc_tests.exe";                     DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#BinDir}\tools\device_probe\Release\hc_device_probe.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "..\tests\data\*";                                         DestDir: "{app}\bin\testdata"; Flags: ignoreversion
Source: "..\assets\*";                                             DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\assets\*";                                             DestDir: "{app}\bin\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\assets\fonts\OFL.txt";                                  DestDir: "{app}\licenses"; DestName: "Poppins-OFL.txt"; Flags: ignoreversion
Source: "CONSOLE-HOW-TO-TEST.txt";                                  DestDir: "{app}"; DestName: "HOW-TO-TEST.txt"; Flags: ignoreversion
Source: "CONSOLE-THIRD-PARTY-NOTICES.txt";                          DestDir: "{app}"; DestName: "THIRD-PARTY-NOTICES.txt"; Flags: ignoreversion
Source: "..\LICENSE";                                              DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
#ifdef AsioLicense
Source: "{#AsioLicense}";                                          DestDir: "{app}\licenses"; DestName: "Steinberg-ASIO-SDK-LICENSE.txt"; Flags: ignoreversion
#endif
Source: "..\external\dr_libs\LICENSE";                             DestDir: "{app}\licenses"; DestName: "dr_libs-LICENSE.txt"; Flags: ignoreversion
Source: "..\external\stb\LICENSE";                                  DestDir: "{app}\licenses"; DestName: "stb-LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\HEM Digital Console";            Filename: "{app}\HEMDigitalConsole.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\HEM Digital Console";      Filename: "{app}\HEMDigitalConsole.exe"; WorkingDir: "{app}"
; Diagnostics. cmd /k keeps the window open so the results can be read.
Name: "{group}\Diagnostics\1 - Rig Check";           Filename: "{cmd}"; Parameters: "/k """"{app}\bin\hc_device_probe.exe"" --seconds 2"""; WorkingDir: "{app}\bin"
Name: "{group}\Diagnostics\2 - Engine Self-Test";    Filename: "{cmd}"; Parameters: "/k """"{app}\bin\hc_tests.exe"""""; WorkingDir: "{app}\bin"
Name: "{group}\Diagnostics\3 - Screen Snapshots";    Filename: "{cmd}"; Parameters: "/k """"{app}\bin\hem_console_snapshot.exe"" --out ""%USERPROFILE%\Desktop\HEM Console Snapshots"""""; WorkingDir: "{app}\bin"
Name: "{group}\How to test";                    Filename: "{app}\HOW-TO-TEST.txt"
Name: "{group}\Uninstall HEM Digital Console";  Filename: "{uninstallexe}"

[Registry]
; .hemc session files open in the HEM Digital Console (double-click in Explorer).
Root: HKA; Subkey: "Software\Classes\.hemc";                                    ValueType: string; ValueName: "";                      ValueData: "HEM.DigitalConsole.Session"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.hemc\OpenWithProgids";                   ValueType: string; ValueName: "HEM.DigitalConsole.Session"; ValueData: "";          Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\HEM.DigitalConsole.Session";                ValueType: string; ValueName: "";                      ValueData: "HEM Digital Console Session"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\HEM.DigitalConsole.Session\DefaultIcon";   ValueType: string; ValueName: "";                      ValueData: "{app}\HEMDigitalConsole.exe,0"
Root: HKA; Subkey: "Software\Classes\HEM.DigitalConsole.Session\shell\open\command"; ValueType: string; ValueName: "";                 ValueData: """{app}\HEMDigitalConsole.exe"" ""%1"""

[Run]
Filename: "{app}\HOW-TO-TEST.txt";        Description: "Show how to test"; Flags: postinstall shellexec skipifsilent unchecked
Filename: "{app}\HEMDigitalConsole.exe";  Description: "Start the HEM Digital Console"; Flags: postinstall nowait skipifsilent
