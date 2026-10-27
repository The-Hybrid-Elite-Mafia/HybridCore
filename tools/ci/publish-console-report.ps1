<#
  CI helper: runs hc_tests.exe and publishes every HEM Digital Console line (the [console: ...]
  sections and the [console benchmark]) as GitHub "notice" annotations, split into parts that fit
  the ~4 KB annotation limit, so the full measured results can be read back through the
  check-runs API. Exits with hc_tests' own exit code.
#>
param([Parameter(Mandatory = $true)][string]$Exe)

$ErrorActionPreference = 'Continue'
$lines = & $Exe 2>&1 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
if ($null -eq $code) { $code = 0 }

$keep = New-Object System.Collections.Generic.List[string]
$inConsole = $false
foreach ($l in $lines) {
    if ($l -match '^\[') { $inConsole = $l -match '^\[console' }
    if ($inConsole -or $l -match 'checks, \d+ failed') { $keep.Add($l) }
}

$parts = New-Object System.Collections.Generic.List[string]
$cur = ''
foreach ($l in $keep) {
    if (($cur.Length + $l.Length + 1) -gt 3800) { $parts.Add($cur); $cur = '' }
    $cur += $l + "`n"
}
if ($cur.Length) { $parts.Add($cur) }

for ($i = 0; $i -lt $parts.Count; $i++) {
    $enc = $parts[$i].Replace('%', '%25').Replace("`r", '').Replace("`n", '%0A')
    Write-Host "::notice title=console report $($i + 1) of $($parts.Count) (exit $code)::$enc"
}
exit $code
