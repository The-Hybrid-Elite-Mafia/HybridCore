<#
  CI: packs the complete source code of this HEM Digital Console build next to its installer, as the GPL asks:
  this repository (The Hybrid Core + the console) at the built commit, plus the Steinberg ASIO SDK as built
  (GPLv3 option, with the one-line asiolist.cpp fix). Sets HEM_CONSOLE_SOURCE_ZIP for the release step.
#>
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$ver = $env:HEM_CONSOLE_VERSION
if (-not $ver) { throw 'HEM_CONSOLE_VERSION is not set (run build-console-installer.ps1 first)' }

$work  = Join-Path ([IO.Path]::GetTempPath()) ('hem-console-src-' + [Guid]::NewGuid().ToString('N'))
$stage = Join-Path $work "HEMDigitalConsole-$ver-source"
New-Item -ItemType Directory -Force -Path $stage | Out-Null
$tar = Join-Path $work 'src.tar'
git -C $root archive --format=tar -o $tar HEAD
if ($LASTEXITCODE -ne 0) { throw 'git archive failed' }
tar -xf $tar -C $stage
if ($LASTEXITCODE -ne 0) { throw 'tar failed' }

$asio = Join-Path $root 'external\asiosdk\sdk'
if (-not (Test-Path (Join-Path $asio 'common\asio.h'))) { throw "ASIO SDK not found at $asio" }
Copy-Item -Recurse -Force $asio (Join-Path $stage 'external\asiosdk\sdk')

@"
HEM Digital Console $ver - complete source code of the installer released with it.

The HEM Digital Console and The Hybrid Core: GNU General Public License v3 or later (LICENSE).
Copyright (C) 2026 Steven Reliford / Hybrid Elite Mafia.

external\asiosdk\sdk - Steinberg ASIO SDK 2.3.4, used under its GPLv3 option (its LICENSE.txt).
  Downloaded from download.steinberg.net (SHA-512 checked); the only change is in host\pc\asiolist.cpp,
  where 'delete lpdrv;' became 'delete[] lpdrv;' (memory allocated with new[]).
  ASIO is a trademark of Steinberg Media Technologies GmbH.

To build (Windows, Visual Studio 2022, CMake 3.28+):
  cmake --preset rig-asio
  cmake --build --preset rig-asio-release --target HEMDigitalConsole
(tools\fetch-asio-sdk.ps1 fetches the ASIO SDK again if it is ever missing.)
"@ | Out-File -FilePath (Join-Path $stage 'SOURCE-README.txt') -Encoding utf8

$zip = Join-Path $root "installer\output\HEMDigitalConsole-$ver-source.zip"
New-Item -ItemType Directory -Force -Path (Split-Path $zip) | Out-Null
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
$f = Get-Item $zip
Write-Host ("Source: {0}  ({1:N1} MB)" -f $f.Name, ($f.Length / 1MB))
"HEM_CONSOLE_SOURCE_ZIP=$($f.FullName)" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
exit 0
