<#
  CI: builds the Inno Setup installer from the rig-asio Release build.
  Version = tag without the leading "v" on tag builds, else 0.0.0-dev.<run number>.
#>
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')

if ($env:GITHUB_REF -like 'refs/tags/v*') { $ver = $env:GITHUB_REF_NAME.Substring(1) }
else { $ver = "0.0.0-dev.$($env:GITHUB_RUN_NUMBER)" }

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
