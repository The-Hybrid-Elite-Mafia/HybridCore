<#
  CI: runs Steinberg's VST3 validator (built from the SDK) on every plug-in bundle:
  The Hybrid Core, HEM Suite (the HEM effects) and HEM WAVE (the synth).
#>
param([Parameter(Mandatory = $true)][string]$BuildDir)
$ErrorActionPreference = 'Stop'
$validator = Get-ChildItem $BuildDir -Recurse -Filter 'validator.exe' -File | Where-Object { $_.FullName -match '\\Release\\' } | Select-Object -First 1
if (-not $validator) { throw 'validator.exe not found' }
Write-Host "validator: $($validator.FullName)"
$worst = 0
$totalOk = 0
$totalBad = 0
foreach ($name in @('The Hybrid Core.vst3', 'HEM Suite.vst3', 'HEM WAVE.vst3')) {
    $bundle = Get-ChildItem $BuildDir -Recurse -Directory -Filter $name | Where-Object { $_.FullName -match '\\Release\\' } | Select-Object -First 1
    if (-not $bundle) { $bundle = Get-ChildItem $BuildDir -Recurse -Directory -Filter $name | Select-Object -First 1 }
    if (-not $bundle) { throw "$name bundle not found" }
    Write-Host ""
    Write-Host "plug-in  : $($bundle.FullName)"
    $out  = & $validator.FullName $bundle.FullName 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host $_ }
    $ok   = ($out | Select-String -SimpleMatch '[Succeeded]').Count
    $bad  = ($out | Select-String -SimpleMatch '[Failed]').Count
    Write-Host "VALIDATOR $name : $ok test(s) succeeded, $bad failed, exit code $code"
    $totalOk += $ok
    $totalBad += $bad
    if ($code -ne 0) { $worst = $code }
}
Write-Host ""
Write-Host "VALIDATOR SUMMARY: $totalOk test(s) succeeded, $totalBad failed, exit code $worst"
exit $worst
