<#
  CI (main only): publishes the installer as GitHub Release v<VERSION>.
  The release for the current VERSION always holds the newest passing build of main:
  if it already exists it is replaced (release + tag recreated at this commit).
  Bump VERSION to start a new release line.
#>
$ErrorActionPreference = 'Stop'
$tag = "v$($env:HC_VERSION)"

gh release view $tag *> $null
if ($LASTEXITCODE -eq 0) {
    Write-Host "Release $tag exists; replacing it with this build ($env:GITHUB_SHA)."
    gh release delete $tag --yes --cleanup-tag
    if ($LASTEXITCODE -ne 0) { throw "gh release delete failed ($LASTEXITCODE)" }
}

$assets = @("$($env:HC_INSTALLER)")
gh release create $tag @assets --target "$($env:GITHUB_SHA)" `
    --title "The Hybrid Core $tag" --notes-file installer/RELEASE-NOTES.md
if ($LASTEXITCODE -ne 0) { throw "gh release create failed ($LASTEXITCODE)" }
Write-Host "Published release $tag with $(Split-Path $env:HC_INSTALLER -Leaf) at $env:GITHUB_SHA"
