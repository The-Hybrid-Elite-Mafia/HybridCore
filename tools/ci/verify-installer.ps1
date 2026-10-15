<#
  CI: proves the installer works on a clean Windows machine.
  install (silent) -> files + shortcuts present -> installed self-test passes ->
  settings folder survives an upgrade-style reinstall and an uninstall.
#>
param([Parameter(Mandatory = $true)][string]$Installer)
$ErrorActionPreference = 'Stop'

$app      = Join-Path $env:ProgramFiles 'Hybrid Elite Mafia\The Hybrid Core'
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
foreach ($f in 'bin\hc_device_probe.exe','bin\hc_engine_smoke.exe','bin\hc_tests.exe','HOW-TO-TEST.txt','THIRD-PARTY-NOTICES.txt','unins000.exe') {
    Check (Test-Path (Join-Path $app $f)) "installed $f"
}
Check (Test-Path (Join-Path $desktop 'Hybrid Core - Rig Check.lnk')) 'desktop shortcut'
Check (Test-Path $settings) 'settings folder created'

$out = & (Join-Path $app 'bin\hc_tests.exe') 2>&1 | Out-String
Check ($LASTEXITCODE -eq 0 -and $out -match '0 failed -> PASS') 'installed hc_tests.exe passes'
($out -split "`n" | Select-Object -Last 2) -join ' ' | Write-Host

# Settings must survive reinstall (upgrade path) and uninstall.
$marker = Join-Path $settings 'ci-marker.txt'
'keep me' | Out-File $marker -Encoding ascii
Check ((Install) -eq 0) 'reinstall over existing (upgrade path) exit code 0'
Check (Test-Path $marker) 'settings kept across reinstall'

$p = Start-Process -FilePath (Join-Path $app 'unins000.exe') -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART' -Wait -PassThru
Start-Sleep -Seconds 3
Check ($p.ExitCode -eq 0) 'silent uninstall exit code 0'
Check (-not (Test-Path (Join-Path $app 'bin\hc_tests.exe'))) 'program files removed on uninstall'
Check (Test-Path $marker) 'settings kept after uninstall'

Write-Host ""
Write-Host "installer verification: $fails failure(s)"
exit $fails
