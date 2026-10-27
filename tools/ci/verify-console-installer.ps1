<#
  CI: proves the HEM Digital Console installer works on a clean Windows machine:
  install (silent) -> files, shortcut, .hemc association -> installed self-test passes -> the installed app starts,
  draws its window and writes its settings -> settings survive a reinstall (upgrade) and an uninstall ->
  uninstall removes the program files.
#>
param([Parameter(Mandatory = $true)][string]$Installer)
$ErrorActionPreference = 'Stop'
$app      = Join-Path $env:ProgramFiles 'Hybrid Elite Mafia\HEM Digital Console'
$settings = Join-Path $env:APPDATA 'HEM Digital Console'
$desktop  = [Environment]::GetFolderPath('CommonDesktopDirectory')
$fails    = 0
function Check([bool]$ok, [string]$what) { if ($ok) { Write-Host "PASS  $what" } else { Write-Host "FAIL  $what"; $script:fails++ } }
function Install() { (Start-Process -FilePath $Installer -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-' -Wait -PassThru).ExitCode }

Check ((Install) -eq 0) 'silent install exit code 0'
foreach ($f in 'HEMDigitalConsole.exe','bin\hem_console_snapshot.exe','bin\hc_tests.exe','bin\hc_device_probe.exe',
              'assets\faceplates\Main-Faceplate.png','assets\fonts\Poppins-Bold.ttf','HOW-TO-TEST.txt','THIRD-PARTY-NOTICES.txt',
              'licenses\Poppins-OFL.txt','unins000.exe') {
    Check (Test-Path (Join-Path $app $f)) "installed $f"
}
Check (Test-Path (Join-Path $desktop 'HEM Digital Console.lnk')) 'desktop shortcut'
$cmd = (Get-ItemProperty -Path 'Registry::HKEY_LOCAL_MACHINE\Software\Classes\HEM.DigitalConsole.Session\shell\open\command' -ErrorAction SilentlyContinue).'(default)'
Check ($cmd -like '*HEMDigitalConsole.exe*%1*') ".hemc sessions open with HEMDigitalConsole.exe ($cmd)"

$out = & (Join-Path $app 'bin\hc_tests.exe') 2>&1 | Out-String
Check ($LASTEXITCODE -eq 0 -and $out -match '0 failed -> PASS') 'installed hc_tests.exe passes'
($out -split "`n" | Select-Object -Last 2) -join ' ' | Write-Host

# The runner has no GPU: Mesa's software OpenGL next to the installed exe (CI only, never in the installer).
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'fetch-mesa.ps1') -TargetDirs $app | Out-Host
Remove-Item -Recurse -Force $settings -ErrorAction SilentlyContinue
$shot = Join-Path $env:RUNNER_TEMP 'console_installed.png'
$p = Start-Process -FilePath (Join-Path $app 'HEMDigitalConsole.exe') -ArgumentList '--exit-after','8','--device','offline','--screenshot',"`"$shot`"" -PassThru
if (-not $p.WaitForExit(90000)) { $p.Kill() }
Check ($p.ExitCode -eq 0) "installed app ran and exited cleanly (exit $($p.ExitCode))"
Check (Test-Path $shot) 'installed app drew its window (screenshot)'
Check (Test-Path (Join-Path $settings 'settings.ini')) 'settings written to %APPDATA%\HEM Digital Console'
Check (Test-Path (Join-Path $settings 'autosave.hemc')) 'last session kept (autosave.hemc)'

$marker = Join-Path $settings 'ci-marker.txt'
'keep me' | Out-File $marker -Encoding ascii
Check ((Install) -eq 0) 'reinstall over existing (upgrade path) exit code 0'
Check (Test-Path $marker) 'settings kept across reinstall'
$u = Start-Process -FilePath (Join-Path $app 'unins000.exe') -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART' -Wait -PassThru
Start-Sleep -Seconds 3
Check ($u.ExitCode -eq 0) 'silent uninstall exit code 0'
Check (-not (Test-Path (Join-Path $app 'HEMDigitalConsole.exe'))) 'program files removed on uninstall'
Check (-not (Test-Path 'Registry::HKEY_LOCAL_MACHINE\Software\Classes\HEM.DigitalConsole.Session')) '.hemc association removed on uninstall'
Check (Test-Path $marker) 'settings kept after uninstall'
Write-Host ""
Write-Host "console installer verification: $fails failure(s)"
exit $fails
