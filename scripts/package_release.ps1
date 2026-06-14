param(
    [switch]$SkipFlutterBuild,
    [string]$Flutter = "F:\Projects\Flutter\Repos\flutter\bin\flutter.bat",
    [string]$OutputDir = ".omo\release"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$GuiDir = Join-Path $RepoRoot "gui"
$ReleaseDir = Join-Path $RepoRoot $OutputDir
$GuiReleaseDir = Join-Path $GuiDir "build\windows\x64\runner\Release"
$AddonDir = Join-Path $RepoRoot "blender\dlss_compositor"
$GuiStageDir = Join-Path $ReleaseDir "_gui_zip_stage"
$StageDir = Join-Path $ReleaseDir "_blender_zip_stage"

New-Item -ItemType Directory -Force -Path $ReleaseDir | Out-Null

if (-not $SkipFlutterBuild) {
    Push-Location $GuiDir
    try {
        & $Flutter pub get
        & $Flutter build windows --release
    } finally {
        Pop-Location
    }
}

if (-not (Test-Path $GuiReleaseDir)) {
    throw "GUI release directory not found: $GuiReleaseDir"
}

$GuiZip = Join-Path $ReleaseDir "gui-windows.zip"
if (Test-Path $GuiZip) {
    Remove-Item -LiteralPath $GuiZip -Force
}
if (Test-Path $GuiStageDir) {
    Remove-Item -LiteralPath $GuiStageDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $GuiStageDir | Out-Null
$excludedGuiExtensions = @(".lib", ".pdb", ".exp", ".ilk")
Get-ChildItem -LiteralPath $GuiReleaseDir -Recurse -File | ForEach-Object {
    if ($excludedGuiExtensions -contains $_.Extension.ToLowerInvariant()) {
        return
    }
    $relative = $_.FullName.Substring($GuiReleaseDir.Length).TrimStart('\', '/')
    $target = Join-Path $GuiStageDir $relative
    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $target -Force
}
Compress-Archive -Path (Join-Path $GuiStageDir "*") -DestinationPath $GuiZip -Force
Remove-Item -LiteralPath $GuiStageDir -Recurse -Force

if (Test-Path $StageDir) {
    Remove-Item -LiteralPath $StageDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

$excludedDirs = @("__pycache__", "tests", ".git")
$excludedExtensions = @(".pyc", ".zip")
Get-ChildItem -LiteralPath $AddonDir -Recurse -File | ForEach-Object {
    $relative = $_.FullName.Substring($AddonDir.Length).TrimStart('\', '/')
    $parts = $relative -split '[\\/]'
    if ($parts | Where-Object { $excludedDirs -contains $_ }) {
        return
    }
    if ($excludedExtensions -contains $_.Extension.ToLowerInvariant()) {
        return
    }
    $target = Join-Path $StageDir $relative
    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $target -Force
}

$manifest = Get-Content (Join-Path $AddonDir "blender_manifest.toml") -Raw
$version = if ($manifest -match '(?m)^version\s*=\s*"([^"]+)"') { $Matches[1] } else { "dev" }
$BlenderZip = Join-Path $ReleaseDir "blender-dlss-compositor-v$version.zip"
if (Test-Path $BlenderZip) {
    Remove-Item -LiteralPath $BlenderZip -Force
}
Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $BlenderZip -Force
Remove-Item -LiteralPath $StageDir -Recurse -Force

Write-Host "Created $GuiZip"
Write-Host "Created $BlenderZip"
