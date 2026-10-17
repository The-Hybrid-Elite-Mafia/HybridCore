; =============================================================================
;  The Hybrid Core — Windows installer (Inno Setup 6)
;  Built by CI:  ISCC /DAppVersion=x.y.z /DBinDir=<build dir> [/DVst3Bundle=<path>] [/DAsioLicense=<path>] HybridCore.iss
;  AppId is permanent: never change it, or upgrades will install side-by-side.
; =============================================================================
#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef BinDir
  #define BinDir "..\build\rig-full"
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
SetupIconFile=..\app\resources\HybridCore.ico
UninstallDisplayIcon={app}\HybridCore.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
PrivilegesRequired=admin
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=The Hybrid Core
CloseApplications=yes
ChangesAssociations=yes

; Settings: the app keeps them in %APPDATA%\HybridCore (settings.ini, skins\, texcache\).
; This installer never creates, overwrites or deletes that folder, so settings survive
; upgrades and uninstall.

[Tasks]
#ifdef Vst3Bundle
Name: "vst3"; Description: "Install the VST3 plug-ins (The Hybrid Core, the HEM effects, the HEM WAVE synth) into Common Files\VST3"
#endif

[Files]
Source: "{#BinDir}\app\Release\HybridCore.exe";                     DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\tools\device_probe\Release\hc_device_probe.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#BinDir}\tools\engine_smoke\Release\hc_engine_smoke.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#BinDir}\tests\Release\hc_tests.exe";                     DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#BinDir}\tools\ui_snapshot\Release\hc_ui_snapshot.exe";   DestDir: "{app}\bin"; Flags: ignoreversion
Source: "..\tests\data\*";                                         DestDir: "{app}\bin\testdata"; Flags: ignoreversion
Source: "..\assets\*";                                             DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\assets\fonts\OFL.txt";                                  DestDir: "{app}\licenses"; DestName: "Poppins-OFL.txt"; Flags: ignoreversion
Source: "HOW-TO-TEST.txt";                                          DestDir: "{app}"; Flags: ignoreversion
Source: "THIRD-PARTY-NOTICES.txt";                                  DestDir: "{app}"; Flags: ignoreversion
Source: "..\external\dr_libs\LICENSE";                             DestDir: "{app}\licenses"; DestName: "dr_libs-LICENSE.txt"; Flags: ignoreversion
Source: "..\external\stb\LICENSE";                                  DestDir: "{app}\licenses"; DestName: "stb-LICENSE.txt"; Flags: ignoreversion
#ifdef AsioLicense
Source: "{#AsioLicense}"; DestDir: "{app}\licenses"; DestName: "Steinberg-ASIO-SDK-LICENSE.txt"; Flags: ignoreversion
#endif
#ifdef Vst3Bundle
Source: "{#Vst3Bundle}\*"; DestDir: "{commoncf64}\VST3\The Hybrid Core.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs; Tasks: vst3
Source: "{#Vst3License}";  DestDir: "{app}\licenses"; DestName: "Steinberg-VST3-SDK-LICENSE.txt"; Flags: ignoreversion
#endif
#ifdef HemSuiteBundle
Source: "{#HemSuiteBundle}\*"; DestDir: "{commoncf64}\VST3\HEM Suite.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs; Tasks: vst3
#endif
#ifdef HemWaveBundle
Source: "{#HemWaveBundle}\*"; DestDir: "{commoncf64}\VST3\HEM WAVE.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs; Tasks: vst3
#endif

[Icons]
Name: "{group}\The Hybrid Core";               Filename: "{app}\HybridCore.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\The Hybrid Core";         Filename: "{app}\HybridCore.exe"; WorkingDir: "{app}"
; Diagnostics. cmd /k keeps the window open so the results can be read.
Name: "{group}\Diagnostics\1 - Rig Check";             Filename: "{cmd}"; Parameters: "/k """"{app}\bin\hc_device_probe.exe"" --seconds 2"""; WorkingDir: "{app}\bin"
Name: "{group}\Diagnostics\2 - Mixer Test (WASAPI)";   Filename: "{cmd}"; Parameters: "/k """"{app}\bin\hc_engine_smoke.exe"" --mixer --seconds 15"""; WorkingDir: "{app}\bin"
Name: "{group}\Diagnostics\3 - Mixer Test (ASIO)";     Filename: "{cmd}"; Parameters: "/k """"{app}\bin\hc_engine_smoke.exe"" --asio --mixer --seconds 15"""; WorkingDir: "{app}\bin"
Name: "{group}\Diagnostics\4 - Engine Self-Test";      Filename: "{cmd}"; Parameters: "/k """"{app}\bin\hc_tests.exe"""""; WorkingDir: "{app}\bin"
Name: "{group}\Diagnostics\5 - Screen Snapshots";      Filename: "{cmd}"; Parameters: "/k """"{app}\bin\hc_ui_snapshot.exe"""""; WorkingDir: "{app}\bin"
Name: "{group}\How to test";                   Filename: "{app}\HOW-TO-TEST.txt"
Name: "{group}\Uninstall The Hybrid Core";     Filename: "{uninstallexe}"

[Registry]
; .hcs session files open in The Hybrid Core (double-click in Explorer).
Root: HKA; Subkey: "Software\Classes\.hcs";                                  ValueType: string; ValueName: "";                   ValueData: "HybridCore.Session"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.hcs\OpenWithProgids";                 ValueType: string; ValueName: "HybridCore.Session"; ValueData: "";                   Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\HybridCore.Session";                     ValueType: string; ValueName: "";                   ValueData: "Hybrid Core Session"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\HybridCore.Session\DefaultIcon";        ValueType: string; ValueName: "";                   ValueData: "{app}\HybridCore.exe,0"
Root: HKA; Subkey: "Software\Classes\HybridCore.Session\shell\open\command"; ValueType: string; ValueName: "";                   ValueData: """{app}\HybridCore.exe"" ""%1"""

[Run]
Filename: "{app}\HOW-TO-TEST.txt"; Description: "Show how to test"; Flags: postinstall shellexec skipifsilent unchecked
Filename: "{app}\HybridCore.exe";  Description: "Start The Hybrid Core"; Flags: postinstall nowait skipifsilent
