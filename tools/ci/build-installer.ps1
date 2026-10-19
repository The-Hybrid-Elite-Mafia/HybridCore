<#
  CI: builds the Inno Setup installer from a Release build (app, tools, VST3 plug-ins).
  Version comes from the VERSION file at the repo root.
  Builds of main use it as-is; any other branch gets a "-dev.<run number>" suffix.

    -BuildDir build\rig-public   the PUBLIC installer (default): must contain no ASIO code
    -BuildDir build\rig-full -PrivateAsio
                                 Steven's own ASIO build: HybridCore-Setup-x.y.z-PRIVATE-ASIO.exe,
                                 published only in the private HybridCore-Private-Builds repository
#>
param([string]$BuildDir = 'build\rig-public', [switch]$PrivateAsio)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$bin  = Join-Path $root $BuildDir

$base = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
if ($base -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION must be x.y.z, got '$base'" }
if ($env:GITHUB_REF -eq 'refs/heads/main') { $ver = $base }
else { $ver = "$base-dev.$($env:GITHUB_RUN_NUMBER)" }

$iscc = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'
if (-not (Test-Path $iscc)) {
    Write-Host 'Inno Setup not found on runner; installing via Chocolatey.'
    choco install innosetup -y --no-progress | Out-Host
}
if (-not (Test-Path $iscc)) { throw "ISCC.exe not found at $iscc" }

$defs = @("/DAppVersion=$ver", "/DBinDir=$bin")
# ASIO check: the public installer must not contain ASIO code; the private one must.
$exe = Join-Path $bin 'app\Release\HybridCore.exe'
if (-not (Test-Path $exe)) { throw "HybridCore.exe not found at $exe" }
$hasAsio = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($exe)).Contains('ASIO open failed at')
if ($PrivateAsio) {
    if (-not $hasAsio) { throw 'PrivateAsio build requested but HybridCore.exe has no ASIO code' }
    $lic = Join-Path $root 'external\asiosdk\sdk\LICENSE.txt'
    if (-not (Test-Path $lic)) { throw "ASIO SDK LICENSE.txt not found at $lic" }
    $defs += "/DAsioLicense=$lic", '/DPrivateAsio'
    Write-Host 'PRIVATE build: ASIO included (never publish this installer publicly).'
} else {
    if ($hasAsio) { throw 'PUBLIC installer refused: HybridCore.exe contains ASIO code. Build with --preset rig-public.' }
    Write-Host 'PUBLIC build: no ASIO code in HybridCore.exe (checked).'
}

$bundle = Get-ChildItem $bin -Recurse -Directory -Filter 'The Hybrid Core.vst3' -ErrorAction SilentlyContinue | Select-Object -First 1
$vstLic = Get-ChildItem (Join-Path $bin '_deps') -Recurse -Filter 'LICENSE.txt' -ErrorAction SilentlyContinue |
          Where-Object { $_.Directory.Name -eq 'vst3sdk-src' } | Select-Object -First 1
if ($bundle -and $vstLic) {
    $defs += "/DVst3Bundle=$($bundle.FullName)"
    $defs += "/DVst3License=$($vstLic.FullName)"
    Write-Host "VST3 bundle: $($bundle.FullName)"
    foreach ($pair in @(@('HEM Suite.vst3', 'HemSuiteBundle'), @('HEM WAVE.vst3', 'HemWaveBundle'))) {
        $b = Get-ChildItem $bin -Recurse -Directory -Filter $pair[0] -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match '\\Release\\' } | Select-Object -First 1
        if ($b) { $defs += "/D$($pair[1])=$($b.FullName)"; Write-Host "VST3 bundle: $($b.FullName)" }
        else { throw "$($pair[0]) not found in the build" }
    }
} else { Write-Host 'NOTE: no VST3 bundle found; installer will not contain the plug-in.' }

& $iscc @defs (Join-Path $root 'installer\HybridCore.iss') | Out-Host
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }

$exe = Get-ChildItem (Join-Path $root 'installer\output') -Filter 'HybridCore-Setup-*.exe' | Select-Object -First 1
Write-Host ("Installer: {0}  ({1:N1} MB)" -f $exe.Name, ($exe.Length / 1MB))
Write-Host ("SHA-256: {0}" -f (Get-FileHash $exe.FullName -Algorithm SHA256).Hash.ToLowerInvariant())
"HC_INSTALLER=$($exe.FullName)" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
"HC_VERSION=$ver"               | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
