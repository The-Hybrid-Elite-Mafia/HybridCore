<#
  CI (main only): publishes the installer as GitHub Release v<VERSION> if that release
  does not exist yet. Re-runs for the same VERSION do nothing, so bump VERSION to ship.
#>
$ErrorActionPreference = 'Stop'
$tag = "v$($env:HC_VERSION)"

gh release view $tag *> $null
if ($LASTEXITCODE -eq 0) {
    Write-Host "Release $tag already exists; nothing to publish. Bump VERSION to release a new build."
    exit 0
}

gh release create $tag "$($env:HC_INSTALLER)" --target "$($env:GITHUB_SHA)" `
    --title "The Hybrid Core $tag" --notes-file installer/RELEASE-NOTES.md
if ($LASTEXITCODE -ne 0) { throw "gh release create failed ($LASTEXITCODE)" }
Write-Host "Published release $tag with $(Split-Path $env:HC_INSTALLER -Leaf)"
