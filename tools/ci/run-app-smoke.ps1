<#
  CI: starts the real HybridCore.exe (GUI) with Mesa software OpenGL next to it, lets it run,
  saves a screenshot of its own window, exits by itself, and checks it left settings behind.
  The runner has no sound card, so the app is started on its offline clock (--device offline):
  the real engine runs and the meters move, nothing is audible.
  With -StemsDir, a session file (.hcs) naming two stems from that folder is written and passed
  on the command line, as Explorer does on a double-click: the run must open it (settings show the
  session and both stem slots, and the screenshot shows them loaded).
  With -FxDemo, the FX RACK opens with HEM PUNCH and HEM TUNE on strip 1; settings must keep both.
  With -SavedAsio, settings from an ASIO build (device.kind=asio) are put in place and the app is
  started WITHOUT --device offline: it must start cleanly and fall back to WASAPI (settings then say wasapi).
#>
param([Parameter(Mandatory = $true)][string]$Exe, [Parameter(Mandatory = $true)][string]$Shot, [string]$StemsDir = '', [switch]$FxDemo, [switch]$SavedAsio)
$ErrorActionPreference = 'Stop'
$settings = Join-Path $env:APPDATA 'HybridCore\settings.ini'
Remove-Item $settings -ErrorAction SilentlyContinue
Remove-Item $Shot -ErrorAction SilentlyContinue
$shotFull = [IO.Path]::GetFullPath($Shot)

$argsList = @('--exit-after','10','--device','offline','--screenshot',"`"$shotFull`"")
if ($SavedAsio) {
    New-Item -ItemType Directory -Force -Path (Split-Path $settings) | Out-Null
    [IO.File]::WriteAllLines($settings, @('[app]', 'device.kind=asio', 'device.id=CI Fake ASIO Driver'), (New-Object Text.UTF8Encoding($false)))
    $argsList = @('--exit-after','10','--screenshot',"`"$shotFull`"")
}
$session = ''
if ($FxDemo) { $argsList += '--fx-demo' }
if ($StemsDir) {
    $dir = Join-Path $env:RUNNER_TEMP 'hc-session'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $stems = Get-ChildItem $StemsDir -File | Sort-Object Name | Select-Object -First 2
    $session = Join-Path $dir 'CI Song.hcs'
    $lines = @('[session]', 'format=HybridCore Session', 'version=1', '[stems]')
    for ($i = 0; $i -lt $stems.Count; $i++) { $lines += ('slot{0:D2}.abs={1}' -f ($i + 1), $stems[$i].FullName) }
    $lines += @('[state]', 'source=stems', 'loop=1')
    [IO.File]::WriteAllLines($session, $lines, (New-Object Text.UTF8Encoding($false)))
    $argsList += "`"$session`""
}
$p = Start-Process -FilePath $Exe -ArgumentList $argsList -PassThru
if (-not $p.WaitForExit(60000)) { $p.Kill(); throw 'HybridCore.exe did not exit within 60 s' }
Write-Host "exit code: $($p.ExitCode)"
$ok = $p.ExitCode -eq 0
if (Test-Path $shotFull) { Write-Host ("screenshot: {0} ({1:N0} KB)" -f $shotFull, ((Get-Item $shotFull).Length / 1KB)) } else { Write-Host 'screenshot: MISSING'; $ok = $false }
if (Test-Path $settings) { Write-Host "settings written: $settings"; Get-Content $settings | Select-Object -First 8 | ForEach-Object { Write-Host "  $_" } }
else { Write-Host 'settings.ini: MISSING'; $ok = $false }
if ($session) {
    $text = if (Test-Path $settings) { Get-Content $settings -Raw } else { '' }
    $opened = $text -match [regex]::Escape("session=$session")
    $slots  = ([regex]::Matches($text, '(?m)^slot0[12]=.+\S')).Count
    Write-Host "session opened from the command line: $opened; stem slots filled: $slots"
    if (-not $opened -or $slots -ne 2) { $ok = $false }
}
if ($FxDemo) {
    $text = if (Test-Path $settings) { Get-Content $settings -Raw } else { '' }
    $punch = $text -match '(?m)^stem01\.fx1=HEM PUNCH\|'
    $tune  = $text -match '(?m)^stem01\.fx2=HEM TUNE\|'
    Write-Host "FX RACK saved in settings: HEM PUNCH $punch, HEM TUNE $tune"
    if (-not $punch -or -not $tune) { $ok = $false }
}
if ($SavedAsio) {
    $text = if (Test-Path $settings) { Get-Content $settings -Raw } else { '' }
    $fell = $text -match '(?m)^device\.kind=wasapi\s*$'
    Write-Host "saved ASIO device fell back to WASAPI: $fell"
    if (-not $fell) { $ok = $false }
}
if ($ok) { Write-Host 'APP SMOKE PASS'; exit 0 } else { Write-Host 'APP SMOKE FAIL'; exit 1 }
