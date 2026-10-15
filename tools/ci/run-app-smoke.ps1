<#
  CI: starts the real HybridCore.exe (GUI) with Mesa software OpenGL next to it, lets it run,
  saves a screenshot of its own window, exits by itself, and checks it left settings behind.
  The runner has no sound card, so the app is started on its offline clock (--device offline):
  the real engine runs and the meters move, nothing is audible.
#>
param([Parameter(Mandatory = $true)][string]$Exe, [Parameter(Mandatory = $true)][string]$Shot)
$ErrorActionPreference = 'Stop'
$settings = Join-Path $env:APPDATA 'HybridCore\settings.ini'
Remove-Item $settings -ErrorAction SilentlyContinue
Remove-Item $Shot -ErrorAction SilentlyContinue
$shotFull = [IO.Path]::GetFullPath($Shot)

$p = Start-Process -FilePath $Exe -ArgumentList '--exit-after','10','--device','offline','--screenshot',"`"$shotFull`"" -PassThru
if (-not $p.WaitForExit(60000)) { $p.Kill(); throw 'HybridCore.exe did not exit within 60 s' }
Write-Host "exit code: $($p.ExitCode)"
$ok = $p.ExitCode -eq 0
if (Test-Path $shotFull) { Write-Host ("screenshot: {0} ({1:N0} KB)" -f $shotFull, ((Get-Item $shotFull).Length / 1KB)) } else { Write-Host 'screenshot: MISSING'; $ok = $false }
if (Test-Path $settings) { Write-Host "settings written: $settings"; Get-Content $settings | Select-Object -First 8 | ForEach-Object { Write-Host "  $_" } }
else { Write-Host 'settings.ini: MISSING'; $ok = $false }
if ($ok) { Write-Host 'APP SMOKE PASS'; exit 0 } else { Write-Host 'APP SMOKE FAIL'; exit 1 }
