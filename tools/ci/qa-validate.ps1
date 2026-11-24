<#
  HEM QA pass, stage 2: Steinberg's VST3 validator (built from the SDK with the plug-ins) on each bundle,
  in its extensive mode (-e). Report only: always exits 0. Writes, into -OutDir:
    validator_<bundle>.txt     the validator's full output
    validator_summary.csv      bundle, exit code, tests succeeded, tests failed
    validator_failures.csv     bundle, plug-in class, test name, the line that failed
#>
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string[]]$Bundles,
    [Parameter(Mandatory = $true)][string]$OutDir
)
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$summary  = @('bundle,exit_code,succeeded,failed,mode')
$failures = @('bundle,plugin_class,test,detail')

$validator = Get-ChildItem $BuildDir -Recurse -Filter 'validator.exe' -File | Where-Object { $_.FullName -match '\\Release\\' } | Select-Object -First 1
if (-not $validator) {
    Write-Host 'validator.exe not found (the SDK validator is built with the VST3 target)'
    $summary += "(none),-1,0,0,validator.exe not built"
    $summary | Set-Content -Encoding utf8 (Join-Path $OutDir 'validator_summary.csv')
    $failures | Set-Content -Encoding utf8 (Join-Path $OutDir 'validator_failures.csv')
    exit 0
}
Write-Host "validator: $($validator.FullName)"

foreach ($b in $Bundles) {
    $bundle = Get-ChildItem $BuildDir -Recurse -Directory -Filter $b | Where-Object { $_.FullName -match '\\Release\\' -and $_.FullName -notmatch '\\Contents\\' } | Select-Object -First 1
    $safe = ($b -replace '[^A-Za-z0-9]+', '_')
    if (-not $bundle) {
        Write-Host "$b : NOT BUILT"
        $summary += "`"$b`",-1,0,0,bundle not built"
        continue
    }
    Write-Host "== $b ($($bundle.FullName))"
    $mode = 'extensive (-e)'
    $out  = & $validator.FullName -e $bundle.FullName 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    if (($out | Select-String -SimpleMatch '[Succeeded]').Count -eq 0 -and ($out | Select-String -SimpleMatch '[Failed]').Count -eq 0) {
        # older validators without -e: plain run
        $mode = 'standard'
        $out  = & $validator.FullName $bundle.FullName 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    }
    $out | Set-Content -Encoding utf8 (Join-Path $OutDir "validator_$safe.txt")
    $ok  = ($out | Select-String -SimpleMatch '[Succeeded]').Count
    $bad = ($out | Select-String -SimpleMatch '[Failed]').Count
    Write-Host "$b : $ok succeeded, $bad failed, exit $code ($mode)"
    $summary += "`"$b`",$code,$ok,$bad,$mode"

    # which plug-in class and test each failure belongs to
    $class = ''; $test = ''; $last = ''
    foreach ($line in $out) {
        if ($line -match '^\s*(?:Info:\s*)?(?:Name|Class)\s*[:=]\s*(.+)$') { $class = $Matches[1].Trim() }
        elseif ($line -match 'Testing\s+(?:the\s+)?(?:class|plug-?in)?\s*[:\-]?\s*"?([^"]+)"?') { if ($line -notmatch '\[') { $class = $Matches[1].Trim() } }
        if ($line -match '^\s*\[(?!Succeeded|Failed)([^\]]+)\]\s*$') { $test = $Matches[1].Trim() }
        if ($line -match '\[Failed\]') {
            $d = $last.Replace('"', "'")
            $failures += "`"$b`",`"$class`",`"$test`",`"$d`""
        }
        if ($line.Trim().Length -gt 0 -and $line -notmatch '\[Failed\]') { $last = $line.Trim() }
    }
}
$summary  | Set-Content -Encoding utf8 (Join-Path $OutDir 'validator_summary.csv')
$failures | Set-Content -Encoding utf8 (Join-Path $OutDir 'validator_failures.csv')
exit 0
