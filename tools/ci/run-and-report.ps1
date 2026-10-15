<#
  CI helper: runs a command, prints its full output to the job log, and publishes the
  same output as a GitHub "notice" annotation so it can be read back through the
  check-runs API. Exits with the command's own exit code.

  With -WarningSummary, publishes the count and list of unique MSVC warnings instead
  of the full (very long) build log.
#>
param(
    [Parameter(Mandatory = $true)][string]$Title,
    [Parameter(Mandatory = $true)][string]$Exe,
    [string[]]$ArgList = @(),
    [switch]$WarningSummary
)

$ErrorActionPreference = 'Continue'

function Publish([string]$title, [string]$text) {
    if ($text.Length -gt 60000) { $text = $text.Substring(0, 60000) + "`n...(truncated)" }
    $enc = $text.Replace('%', '%25').Replace("`r", '').Replace("`n", '%0A')
    $t   = $title.Replace('%', '%25').Replace(':', '%3A').Replace(',', '%2C')
    Write-Host "::notice title=$t::$enc"
}

$output = & $Exe @ArgList 2>&1 | ForEach-Object { "$_" } | Out-String
$code   = $LASTEXITCODE
if ($null -eq $code) { $code = 0 }

Write-Host $output

if ($WarningSummary) {
    $warnings = $output -split "`n" |
        Where-Object { $_ -match ': warning [A-Z]+\d+' } |
        ForEach-Object { ($_ -replace '\s*\[[^\]]*\.vcxproj\]\s*$', '').Trim() } |
        Sort-Object -Unique
    $summary = "exit code $code`nunique compiler warnings: $($warnings.Count)`n" + ($warnings -join "`n")
    Publish "$Title (exit $code)" $summary
} else {
    Publish "$Title (exit $code)" $output
}

exit $code
