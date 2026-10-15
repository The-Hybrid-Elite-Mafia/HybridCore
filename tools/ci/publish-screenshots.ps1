<#
  CI only: force-pushes the UI snapshot PNGs to the `ci-screenshots` branch (one commit,
  no history) so they can be reviewed without downloading build artifacts.
#>
param([Parameter(Mandatory = $true)][string]$Folder)
$ErrorActionPreference = 'Stop'

$pngs = Get-ChildItem $Folder -Filter '*.png'
if (-not $pngs) { throw "No PNG files in $Folder" }

$tmp = Join-Path ([IO.Path]::GetTempPath()) 'hc-shots'
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
foreach ($p in $pngs) { Copy-Item $p.FullName $tmp }
"Screenshots from commit $env:GITHUB_SHA (run $env:GITHUB_RUN_NUMBER), rendered by hc_ui_snapshot with Mesa llvmpipe." |
    Out-File (Join-Path $tmp 'README.md') -Encoding utf8

Push-Location $tmp
git init -q
git checkout -q -b ci-screenshots
git config user.name 'hybridcore-ci'
git config user.email 'ci@users.noreply.github.com'
git add -A
git commit -q -m "UI screenshots for $env:GITHUB_SHA"
git push -q -f "https://x-access-token:$($env:GH_TOKEN)@github.com/$($env:GITHUB_REPOSITORY).git" ci-screenshots
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { throw "git push failed ($code)" }
Write-Host "Pushed $($pngs.Count) screenshots to branch ci-screenshots"
