<#
.SYNOPSIS
    Packages DLSS Compositor CLI+GUI and the Blender addon into two distributable ZIPs.

.DESCRIPTION
    Outputs:
      dist/dlss-compositor-v<VERSION>.zip          -- CLI exe + GUI (Electron) + DLSS DLLs + shaders/luts
      dist/dlss-compositor-blender-v<VERSION>.zip  -- Blender addon folder

    Prerequisites:
      - CMake Release build already done:  cmake --build build --config Release
      - GUI already built:                 cd gui && npm run build

.PARAMETER Version
    Override the version string (default: read from gui/package.json).

.PARAMETER SkipGuiBuild
    Skip running "npm run build" in the gui folder.

.PARAMETER SkipCmakeBuild
    Skip checking for the CLI exe (useful for testing packaging only).

.EXAMPLE
    .\scripts\package-release.ps1
    .\scripts\package-release.ps1 -Version "1.0.0"
    .\scripts\package-release.ps1 -SkipGuiBuild
    .\scripts\package-release.ps1 -SkipCmakeBuild
#>

param(
    [string]$Version = '',
    [switch]$SkipGuiBuild,
    [switch]$SkipCmakeBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# -- Resolve repo root (one level up from /scripts) --------------------------
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Split-Path -Parent $ScriptDir

Push-Location $RepoRoot
try {

# -- Version -----------------------------------------------------------------
if (-not $Version) {
    $pkgJson = Get-Content (Join-Path $RepoRoot 'gui\package.json') -Raw | ConvertFrom-Json
    $Version = $pkgJson.version
}
Write-Host "[package-release] Packaging DLSS Compositor v$Version" -ForegroundColor Cyan

# -- Paths -------------------------------------------------------------------
$CliRelease   = Join-Path $RepoRoot 'build\Release'
$GuiDir       = Join-Path $RepoRoot 'gui'
$ElectronDist = Join-Path $GuiDir   'node_modules\electron\dist'
$BlenderAddon = Join-Path $RepoRoot 'blender\dlss_compositor_aov'
$DistDir      = Join-Path $RepoRoot 'dist'

$AppZipName     = "dlss-compositor-v$Version.zip"
$BlenderZipName = "dlss-compositor-blender-v$Version.zip"
$AppZipPath     = Join-Path $DistDir $AppZipName
$BlenderZipPath = Join-Path $DistDir $BlenderZipName

# -- Validate prerequisites --------------------------------------------------
Write-Host "`nChecking prerequisites..." -ForegroundColor Yellow

if (-not $SkipCmakeBuild) {
    $cliExe = Join-Path $CliRelease 'dlss-compositor.exe'
    if (-not (Test-Path $cliExe)) {
        throw "CLI executable not found: $cliExe`nRun: cmake --build build --config Release"
    }
    Write-Host "  OK  CLI exe: $cliExe"
}

if (-not (Test-Path $ElectronDist)) {
    throw "Electron dist not found: $ElectronDist`nRun: cd gui && npm install"
}
Write-Host "  OK  Electron runtime: $ElectronDist"

if (-not (Test-Path $BlenderAddon)) {
    throw "Blender addon folder not found: $BlenderAddon"
}
Write-Host "  OK  Blender addon: $BlenderAddon"

# -- Optionally build the GUI ------------------------------------------------
if (-not $SkipGuiBuild) {
    Write-Host "`nBuilding GUI (electron-vite build)..." -ForegroundColor Yellow
    Push-Location $GuiDir
    try {
        npm run build
        if ($LASTEXITCODE -ne 0) { throw "npm run build failed (exit $LASTEXITCODE)" }
    } finally {
        Pop-Location
    }
    Write-Host "  OK  GUI build complete"
} else {
    Write-Host "`n[SKIP] GUI build (-SkipGuiBuild)" -ForegroundColor DarkGray
}

# Validate GUI build outputs exist
$guiBuildChecks = @(
    (Join-Path $GuiDir 'dist-electron\main.js'),
    (Join-Path $GuiDir 'dist-electron\preload.mjs'),
    (Join-Path $GuiDir 'dist\index.html')
)
foreach ($f in $guiBuildChecks) {
    if (-not (Test-Path $f)) {
        throw "GUI build output missing: $f`nRun: cd gui && npm run build"
    }
}
Write-Host "  OK  GUI build outputs present"

# -- Install production-only node_modules for packaging ---------------------
Write-Host "`nInstalling production node_modules for packaging..." -ForegroundColor Yellow
$ProdModulesDir = Join-Path $RepoRoot '.packaging-prod-modules'
if (Test-Path $ProdModulesDir) {
    Remove-Item $ProdModulesDir -Recurse -Force
}
New-Item -ItemType Directory -Path $ProdModulesDir | Out-Null

# Copy package.json and install only production deps into a clean folder
Copy-Item (Join-Path $GuiDir 'package.json') (Join-Path $ProdModulesDir 'package.json')
Push-Location $ProdModulesDir
try {
    # --prefer-offline uses local npm cache; output suppressed
    $npmOut = npm install --omit=dev --no-package-lock --prefer-offline 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host $npmOut
        throw "npm install --omit=dev failed (exit $LASTEXITCODE)"
    }
} finally {
    Pop-Location
}
$ProdNodeModules = Join-Path $ProdModulesDir 'node_modules'
Write-Host "  OK  Production node_modules ready"

# -- Create staging directory ------------------------------------------------
Write-Host "`nStaging files..." -ForegroundColor Yellow
$StagingDir = Join-Path $RepoRoot '.packaging-staging'
if (Test-Path $StagingDir) {
    Remove-Item $StagingDir -Recurse -Force
}

$AppDir = Join-Path $StagingDir "dlss-compositor-v$Version"
New-Item -ItemType Directory -Path $AppDir | Out-Null

# -- 1. Electron runtime files (flat, beside the exe) -----------------------
$ElectronRuntimeFiles = @(
    'electron.exe',
    'icudtl.dat',
    'LICENSES.chromium.html',
    'libGLESv2.dll',
    'libEGL.dll',
    'vk_swiftshader.dll',
    'vk_swiftshader_icd.json',
    'd3dcompiler_47.dll',
    'ffmpeg.dll',
    'vulkan-1.dll',
    'v8_context_snapshot.bin',
    'snapshot_blob.bin',
    'chrome_100_percent.pak',
    'chrome_200_percent.pak',
    'resources.pak'
)

foreach ($file in $ElectronRuntimeFiles) {
    $src = Join-Path $ElectronDist $file
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $AppDir $file)
    }
}

# Rename electron.exe -> dlss-compositor-gui.exe
$electronExeDst = Join-Path $AppDir 'electron.exe'
if (Test-Path $electronExeDst) {
    Rename-Item $electronExeDst 'dlss-compositor-gui.exe'
}

# Copy locales/ folder
$localesSrc = Join-Path $ElectronDist 'locales'
if (Test-Path $localesSrc) {
    Copy-Item $localesSrc (Join-Path $AppDir 'locales') -Recurse
}

Write-Host "  OK  Electron runtime"

# -- 2. App code -> resources/app/ ------------------------------------------
# Electron looks for resources/app/package.json to find the app entry point.
$AppCodeDir = Join-Path $AppDir 'resources\app'
New-Item -ItemType Directory -Path $AppCodeDir | Out-Null

# dist-electron/ contains main.js and preload.mjs
Copy-Item (Join-Path $GuiDir 'dist-electron') (Join-Path $AppCodeDir 'dist-electron') -Recurse

# dist/ contains the rendered index.html + assets
Copy-Item (Join-Path $GuiDir 'dist') (Join-Path $AppCodeDir 'dist') -Recurse

# node_modules (production-only; electron-store and its deps are not bundled)
Copy-Item $ProdNodeModules (Join-Path $AppCodeDir 'node_modules') -Recurse

# package.json so Electron can resolve the "main" field
Copy-Item (Join-Path $GuiDir 'package.json') (Join-Path $AppCodeDir 'package.json')

Write-Host "  OK  App code (dist-electron, dist, node_modules)"

# -- 3. CLI exe + DLSS DLLs + shaders + luts --------------------------------
if (-not $SkipCmakeBuild) {
    Copy-Item (Join-Path $CliRelease 'dlss-compositor.exe') (Join-Path $AppDir 'dlss-compositor.exe')

    foreach ($dll in @('nvngx_dlssd.dll', 'nvngx_dlssg.dll')) {
        $src = Join-Path $CliRelease $dll
        if (Test-Path $src) {
            Copy-Item $src (Join-Path $AppDir $dll)
        }
    }

    $lutsDir = Join-Path $CliRelease 'luts'
    if (Test-Path $lutsDir) {
        Copy-Item $lutsDir (Join-Path $AppDir 'luts') -Recurse
    }

    $shadersDir = Join-Path $CliRelease 'shaders'
    if (Test-Path $shadersDir) {
        Copy-Item $shadersDir (Join-Path $AppDir 'shaders') -Recurse
    }

    Write-Host "  OK  CLI exe, DLSS DLLs, luts, shaders"
}

# -- Ensure output dist/ folder exists ---------------------------------------
if (-not (Test-Path $DistDir)) {
    New-Item -ItemType Directory -Path $DistDir | Out-Null
}

# -- 4. ZIP: app bundle ------------------------------------------------------
Write-Host "`nCreating app ZIP..." -ForegroundColor Yellow
if (Test-Path $AppZipPath) { Remove-Item $AppZipPath -Force }
Compress-Archive -Path $AppDir -DestinationPath $AppZipPath
$appZipSizeMB = [math]::Round((Get-Item $AppZipPath).Length / 1MB, 1)
Write-Host ("  OK  {0}  ({1} MB)" -f $AppZipName, $appZipSizeMB)

# -- 5. ZIP: Blender addon ---------------------------------------------------
Write-Host "`nCreating Blender addon ZIP..." -ForegroundColor Yellow
if (Test-Path $BlenderZipPath) { Remove-Item $BlenderZipPath -Force }

# Stage the Blender addon without __pycache__
$BlenderStagingDir = Join-Path $StagingDir 'dlss_compositor_aov'
Copy-Item $BlenderAddon $BlenderStagingDir -Recurse
Get-ChildItem $BlenderStagingDir -Recurse -Directory -Filter '__pycache__' |
    Remove-Item -Recurse -Force

# Zip the cleaned folder so it extracts as dlss_compositor_aov/
Compress-Archive -Path $BlenderStagingDir -DestinationPath $BlenderZipPath
$blenderZipSizeMB = [math]::Round((Get-Item $BlenderZipPath).Length / 1MB, 3)
Write-Host ("  OK  {0}  ({1} MB)" -f $BlenderZipName, $blenderZipSizeMB)

# -- Cleanup staging ---------------------------------------------------------
Remove-Item $StagingDir -Recurse -Force
Remove-Item $ProdModulesDir -Recurse -Force

# -- Done --------------------------------------------------------------------
Write-Host "`nDone!" -ForegroundColor Green
Write-Host "   $AppZipPath"
Write-Host "   $BlenderZipPath"

} finally {
    Pop-Location
}
