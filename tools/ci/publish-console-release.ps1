<#
  CI: publishes the HEM Digital Console installer as GitHub Release console-v<console/VERSION>.
  Its own release line, never marked "latest" (that stays The Hybrid Core's). If the release for this version
  exists it is replaced (release + tag recreated at this commit). Bump console/VERSION to start a new one.
#>
$ErrorActionPreference = 'Stop'
$tag = "console-v$($env:HEM_CONSOLE_VERSION)"
gh release view $tag *> $null
if ($LASTEXITCODE -eq 0) {
    Write-Host "Release $tag exists; replacing it with this build ($env:GITHUB_SHA)."
    gh release delete $tag --yes --cleanup-tag
    if ($LASTEXITCODE -ne 0) { throw "gh release delete failed ($LASTEXITCODE)" }
}
$assets = @("$($env:HEM_CONSOLE_INSTALLER)")
if ($env:HEM_CONSOLE_SOURCE_ZIP) { $assets += "$($env:HEM_CONSOLE_SOURCE_ZIP)" } else { throw 'source zip missing (GPL: every release carries its source)' }
gh release create $tag @assets --target "$($env:GITHUB_SHA)" --latest=false `
    --title "HEM Digital Console $($env:HEM_CONSOLE_VERSION)" --notes-file installer/CONSOLE-RELEASE-NOTES.md
if ($LASTEXITCODE -ne 0) { throw "gh release create failed ($LASTEXITCODE)" }
Write-Host "Published release $tag with $(Split-Path $env:HEM_CONSOLE_INSTALLER -Leaf) at $env:GITHUB_SHA"
