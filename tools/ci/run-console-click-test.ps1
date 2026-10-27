<#
  CI: the real HEMDigitalConsole.exe presses every control on every page with real window messages
  (--click-test) and writes one line per control. Prints the report; fails if any control did not respond.
#>
param([Parameter(Mandatory = $true)][string]$Exe, [Parameter(Mandatory = $true)][string]$Report)
$ErrorActionPreference = 'Stop'
$full = [IO.Path]::GetFullPath($Report)
Remove-Item $full -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $Exe -ArgumentList @('--device', 'offline', '--maximized', '--click-test', "`"$full`"") -PassThru
if (-not $p.WaitForExit(600000)) { $p.Kill(); throw 'the click test did not finish within 10 minutes' }
if (Test-Path $full) { Get-Content $full | ForEach-Object { Write-Host $_ } } else { Write-Host 'report: MISSING' }
Write-Host "exit code: $($p.ExitCode)"
if ($p.ExitCode -ne 0 -or -not (Test-Path $full)) { exit 1 }
