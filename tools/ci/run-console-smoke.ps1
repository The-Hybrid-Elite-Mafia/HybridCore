<#
  CI: starts the real HEMDigitalConsole.exe (GUI) with Mesa software OpenGL next to it, on the offline clock
  (the runner has no sound card), lets it run, saves a screenshot of its own window and exits by itself.
  With -StemsDir: a session (.hemc) naming the files in that folder is written and passed on the command line,
  as Explorer does on a double-click. The run must open it: the autosave it leaves behind must list the stems.
#>
param([Parameter(Mandatory = $true)][string]$Exe, [Parameter(Mandatory = $true)][string]$Shot, [string]$StemsDir = '', [int]$Page = -1)
$ErrorActionPreference = 'Stop'
$data = Join-Path $env:APPDATA 'HEM Digital Console'
Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
$shotFull = [IO.Path]::GetFullPath($Shot)
Remove-Item $shotFull -ErrorAction SilentlyContinue
$argsList = @('--exit-after', '12', '--device', 'offline', '--screenshot', "`"$shotFull`"")
if ($Page -ge 0) { $argsList += @('--page', "$Page") }
if ($StemsDir) {
    $dir = Join-Path $env:RUNNER_TEMP 'hem-session'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $stems = Get-ChildItem $StemsDir -File | Where-Object { $_.Extension -in '.wav', '.mp3', '.flac' } | Sort-Object Name | Select-Object -First 13
    $session = Join-Path $dir 'CI Song.hemc'
    $lines = @('[hem-digital-console]', 'format=HEM Digital Console Session', 'version=1', '[stems]')
    for ($i = 0; $i -lt $stems.Count; $i++) { $lines += ('slot{0:D2}.abs={1}' -f ($i + 1), $stems[$i].FullName) }
    $lines += @('[app]', 'listen=0', 'loop=1', '[rig]', 'desk.main_ins=1', 'desk.ch01.eq_in=1', 'desk.ch01.hf=6')
    [IO.File]::WriteAllLines($session, $lines, (New-Object Text.UTF8Encoding($false)))
    $argsList += "`"$session`""
    Write-Host "session with $($stems.Count) stems: $session"
}
$p = Start-Process -FilePath $Exe -ArgumentList $argsList -PassThru
if (-not $p.WaitForExit(120000)) { $p.Kill(); throw 'HEMDigitalConsole.exe did not exit within 120 s' }
Write-Host "exit code: $($p.ExitCode)"
$ok = $p.ExitCode -eq 0
if (Test-Path $shotFull) { Write-Host ("screenshot: {0} ({1:N0} KB)" -f $shotFull, ((Get-Item $shotFull).Length / 1KB)) } else { Write-Host 'screenshot: MISSING'; $ok = $false }
$auto = Join-Path $data 'autosave.hemc'
if (Test-Path $auto) {
    $text = Get-Content $auto -Raw
    $slots = ([regex]::Matches($text, '(?m)^slot\d\d\.abs=.+\S')).Count
    Write-Host "autosave.hemc written: $slots stem slot(s); main insert on: $($text -match '(?m)^desk\.main_ins=1')"
    if ($StemsDir -and ($slots -lt 1 -or $text -notmatch '(?m)^desk\.ch01\.hf=6')) { $ok = $false }
} else { Write-Host 'autosave.hemc: MISSING'; $ok = $false }
if (Test-Path (Join-Path $data 'settings.ini')) { Get-Content (Join-Path $data 'settings.ini') | ForEach-Object { Write-Host "  $_" } } else { $ok = $false }
if (-not $ok) { exit 1 }
