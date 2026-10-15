<#
  CI: proves the installer works on a clean Windows machine.
  install (silent) -> files, shortcuts, VST3 plug-in present -> installed self-test passes ->
  settings folder survives an upgrade-style reinstall and an uninstall -> uninstall removes
  program files and the plug-in.
#>
param([Parameter(Mandatory = $true)][string]$Installer)
$ErrorActionPreference = 'Stop'

$app      = Join-Path $env:ProgramFiles 'Hybrid Elite Mafia\The Hybrid Core'
$vst3     = Join-Path $env:CommonProgramFiles 'VST3\The Hybrid Core.vst3'
$settings = Join-Path $env:APPDATA 'HybridCore'
$desktop  = [Environment]::GetFolderPath('CommonDesktopDirectory')
$fails    = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "PASS  $what" } else { Write-Host "FAIL  $what"; $script:fails++ }
}
function Install() {
    $p = Start-Process -FilePath $Installer -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-' -Wait -PassThru
    return $p.ExitCode
}

Check ((Install) -eq 0) 'silent install exit code 0'
foreach ($f in 'HybridCore.exe','bin\hc_device_probe.exe','bin\hc_engine_smoke.exe','bin\hc_tests.exe','bin\hc_ui_snapshot.exe',
              'assets\faceplates\Main-Faceplate.png','assets\fonts\Poppins-Medium.ttf','assets\fonts\Poppins-SemiBold.ttf',
              'assets\fonts\Poppins-Bold.ttf','HOW-TO-TEST.txt','THIRD-PARTY-NOTICES.txt','licenses\stb-LICENSE.txt','licenses\Poppins-OFL.txt','unins000.exe') {
    Check (Test-Path (Join-Path $app $f)) "installed $f"
}
Check (Test-Path (Join-Path $desktop 'The Hybrid Core.lnk')) 'desktop shortcut'
$vstDll = Get-ChildItem $vst3 -Recurse -Filter '*.vst3' -File -ErrorAction SilentlyContinue | Select-Object -First 1
Check ($null -ne $vstDll) "VST3 plug-in installed in $vst3"

$out = & (Join-Path $app 'bin\hc_tests.exe') 2>&1 | Out-String
Check ($LASTEXITCODE -eq 0 -and $out -match '0 failed -> PASS') 'installed hc_tests.exe passes'
($out -split "`n" | Select-Object -Last 2) -join ' ' | Write-Host

New-Item -ItemType Directory -Force -Path $settings | Out-Null   # what the app does on first run
$marker = Join-Path $settings 'ci-marker.txt'
'keep me' | Out-File $marker -Encoding ascii
Check ((Install) -eq 0) 'reinstall over existing (upgrade path) exit code 0'
Check (Test-Path $marker) 'settings kept across reinstall'

$p = Start-Process -FilePath (Join-Path $app 'unins000.exe') -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART' -Wait -PassThru
Start-Sleep -Seconds 3
Check ($p.ExitCode -eq 0) 'silent uninstall exit code 0'
Check (-not (Test-Path (Join-Path $app 'HybridCore.exe'))) 'program files removed on uninstall'
Check (-not (Test-Path $vst3)) 'VST3 plug-in removed on uninstall'
Check (Test-Path $marker) 'settings kept after uninstall'

Write-Host ""
Write-Host "installer verification: $fails failure(s)"
exit $fails
