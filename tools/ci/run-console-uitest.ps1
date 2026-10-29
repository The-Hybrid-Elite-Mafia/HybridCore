<#
  CI: UI automation test of the real HEMDigitalConsole.exe (hem_console_uitest.exe): real clicks and keys
  (SendInput), the Windows file dialogs / "Save changes?" / RECENT menu / folder picker driven with Microsoft UI
  Automation, screen sizes 1366x768, 1920x1080, 2560x1600 and Windows scale 100/150/200 % where Windows allows them
  on the runner, round trip of 50+ changed controls, a crash in the middle of a save, moved stems. Prints the report,
  copies screenshots + report into -Out; fails when any check failed.
#>
param([Parameter(Mandatory = $true)][string]$Exe, [Parameter(Mandatory = $true)][string]$Test, [Parameter(Mandatory = $true)][string]$Out)
$ErrorActionPreference = 'Stop'
$work = Join-Path $env:RUNNER_TEMP 'hem-uitest'
$outFull = [IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Force -Path $outFull | Out-Null
$p = Start-Process -FilePath $Test -ArgumentList @('--app', "`"$([IO.Path]::GetFullPath($Exe))`"", '--work', "`"$work`"", '--out', "`"$outFull`"") -PassThru -NoNewWindow
if (-not $p.WaitForExit(2400000)) { $p.Kill(); Write-Host 'the UI test did not finish within 40 minutes'; exit 1 }
Get-Process HEMDigitalConsole -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host "exit code: $($p.ExitCode)"
if ($p.ExitCode -ne 0) { exit 1 }
