<#
  CI: builds the Inno Setup installer from the rig-asio Release build.
  Version comes from the VERSION file at the repo root. Builds of main use it as-is
  (and become a GitHub Release the first time that version is seen); any other
  branch gets a "-dev.<run number>" suffix.
#>
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')

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

$defs = @("/DAppVersion=$ver", "/DBinDir=$root\build\rig-asio")
$lic  = Join-Path $root 'external\asiosdk\sdk\LICENSE.txt'
if (Test-Path $lic) { $defs += "/DAsioLicense=$lic" } else { Write-Host 'WARNING: ASIO SDK LICENSE.txt not found; not bundled.' }

& $iscc @defs (Join-Path $root 'installer\HybridCore.iss') | Out-Host
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }

$exe = Get-ChildItem (Join-Path $root 'installer\output') -Filter 'HybridCore-Setup-*.exe' | Select-Object -First 1
Write-Host ("Installer: {0}  ({1:N1} MB)" -f $exe.Name, ($exe.Length / 1MB))
"HC_INSTALLER=$($exe.FullName)" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
"HC_VERSION=$ver"               | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
