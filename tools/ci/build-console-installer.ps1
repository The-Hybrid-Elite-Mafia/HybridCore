<#
  CI: builds the HEM Digital Console installer (Inno Setup) from the public Release build (WASAPI only).
  Version: console/VERSION. Builds of main use it as-is; any other branch gets "-dev.<run number>".
#>
param([string]$BuildDir = 'build\rig-public')
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$bin  = Join-Path $root $BuildDir
$base = (Get-Content (Join-Path $root 'console\VERSION') -Raw).Trim()
if ($base -notmatch '^\d+\.\d+\.\d+$') { throw "console/VERSION must be x.y.z, got '$base'" }
if ($env:GITHUB_REF -eq 'refs/heads/main') { $ver = $base } else { $ver = "$base-dev.$($env:GITHUB_RUN_NUMBER)" }
if ($env:HEM_CONSOLE_RELEASE -eq '1') { $ver = $base }

$iscc = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'
if (-not (Test-Path $iscc)) { choco install innosetup -y --no-progress | Out-Host }
if (-not (Test-Path $iscc)) { throw "ISCC.exe not found at $iscc" }

$exe = Join-Path $bin 'console\app\Release\HEMDigitalConsole.exe'
if (-not (Test-Path $exe)) { throw "HEMDigitalConsole.exe not found at $exe" }
if ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($exe)).Contains('ASIO failed to open')) {
    throw 'PUBLIC installer refused: HEMDigitalConsole.exe contains ASIO code. Build with --preset rig-public.'
}
Write-Host 'PUBLIC build: no ASIO code in HEMDigitalConsole.exe (checked).'
& $iscc "/DAppVersion=$ver" "/DBinDir=$bin" (Join-Path $root 'installer\HEMDigitalConsole.iss') | Out-Host
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
$setup = Get-ChildItem (Join-Path $root 'installer\output') -Filter 'HEMDigitalConsole-Setup-*.exe' | Select-Object -First 1
Write-Host ("Installer: {0}  ({1:N1} MB)" -f $setup.Name, ($setup.Length / 1MB))
Write-Host ("SHA-256: {0}" -f (Get-FileHash $setup.FullName -Algorithm SHA256).Hash.ToLowerInvariant())
"HEM_CONSOLE_INSTALLER=$($setup.FullName)" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
"HEM_CONSOLE_VERSION=$ver"                 | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
