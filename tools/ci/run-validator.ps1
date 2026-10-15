<#
  CI: runs Steinberg's VST3 validator (built from the SDK) on The Hybrid Core plug-in.
#>
param([Parameter(Mandatory = $true)][string]$BuildDir)
$ErrorActionPreference = 'Stop'
$validator = Get-ChildItem $BuildDir -Recurse -Filter 'validator.exe' -File | Where-Object { $_.FullName -match '\\Release\\' } | Select-Object -First 1
$bundle    = Get-ChildItem $BuildDir -Recurse -Directory -Filter 'The Hybrid Core.vst3' | Select-Object -First 1
if (-not $validator) { throw 'validator.exe not found' }
if (-not $bundle)    { throw 'The Hybrid Core.vst3 bundle not found' }
Write-Host "validator: $($validator.FullName)"
Write-Host "plug-in  : $($bundle.FullName)"
& $validator.FullName $bundle.FullName
exit $LASTEXITCODE
