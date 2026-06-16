#requires -Version 7.0
[CmdletBinding()]
param(
    [switch]$Windows,
    [switch]$Linux,
    [switch]$Clean,
    [string]$Config = "Release",
    [string]$WslDistro = "Ubuntu-24.04",
    [string]$Generator = "Visual Studio 18 2026"
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$DistWin = Join-Path $Root "dist/windows"
$DistLin = Join-Path $Root "dist/linux"

# Default: build both platforms when no platform switch is given
if (-not ($Windows -or $Linux)) {
    $Windows = $true; $Linux = $true
}

function Write-Step([string]$msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }
function Write-Ok([string]$msg) { Write-Host "  $msg" -ForegroundColor Green }

# Convert a Windows path to its WSL /mnt mount equivalent
function ConvertTo-WslPath([string]$p) {
    $full = (Resolve-Path -LiteralPath $p).Path
    $drive = $full.Substring(0, 1).ToLower()
    $rest = $full.Substring(2) -replace '\\', '/'
    return "/mnt/$drive$rest"
}

# --- Clean ---------------------------------------------------------------
# Remove local build/dist dirs and the WSL build dir
if ($Clean) {
    Write-Step "Clean"
    foreach ($d in @("build", "dist")) {
        $path = Join-Path $Root $d
        if (Test-Path $path) { Remove-Item -Recurse -Force $path; Write-Ok "removed $d/" }
    }
    if ($Linux) {
        wsl.exe -d $WslDistro -e bash -lc "rm -rf ~/bc-build" | Out-Null
        Write-Ok "removed ~/bc-build (WSL)"
    }
}

# --- Windows -------------------------------------------------------------
# Configure + build the native DLL locally via Visual Studio generator
function Build-Windows {
    Write-Step "Windows"
    $build = Join-Path $Root "build"
    cmake -B $build -G $Generator -A x64 -S $Root | Out-Host
    if ($LASTEXITCODE) { throw "cmake configure (windows) failed" }
    cmake --build $build --config $Config | Out-Host
    if ($LASTEXITCODE) { throw "cmake build (windows) failed" }
    $pkg = Join-Path $build "package"
    if (-not (Test-Path "$pkg/addons/BotController/bin/win64/BotController.dll")) {
        throw "windows build produced no BotController.dll"
    }
    Write-Ok "BotController.dll built"
    return $pkg
}

# --- Linux ---------------------------------------------------------------
# Build the .so inside WSL, then stage the package tree back to Windows
function Build-Linux {
    Write-Step "Linux (WSL: $WslDistro)"
    $srcWsl = ConvertTo-WslPath $Root
    $hl2Wsl = ConvertTo-WslPath $env:HL2SDKCS2
    $mmsWsl = ConvertTo-WslPath $env:MMSOURCE_DEV
    $protoWsl = ConvertTo-WslPath $env:CSGO_PROTO
    $bash = @"
set -e
export HL2SDKCS2='$hl2Wsl'
export MMSOURCE_DEV='$mmsWsl'
export CSGO_PROTO='$protoWsl'
cmake -S '$srcWsl' -B ~/bc-build -DCMAKE_BUILD_TYPE=$Config
cmake --build ~/bc-build -j`$(nproc)
test -f ~/bc-build/package/addons/BotController/bin/linuxsteamrt64/BotController.so
echo "BUILD_OK"
"@
    $bash = $bash -replace "`r`n", "`n"
    wsl.exe -d $WslDistro -e bash -lc $bash | Out-Host
    if ($LASTEXITCODE) { throw "WSL linux build failed" }
    # Copy package out of WSL into a Windows staging dir
    $stage = Join-Path $Root "build/linux-package"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force $stage | Out-Null
    $stageWsl = ConvertTo-WslPath $stage
    wsl.exe -d $WslDistro -e bash -lc "cp -r ~/bc-build/package/. '$stageWsl/'" | Out-Host
    if ($LASTEXITCODE) { throw "copying WSL package out failed" }
    Write-Ok "BotController.so built and staged to build/linux-package/"
    return $stage
}

# --- Dist ----------------------------------------------------------------
# Copy the staged addons tree into the per-platform dist folder
function Build-Dist([string]$nativePkg, [string]$destRoot) {
    if (Test-Path $destRoot) { Remove-Item -Recurse -Force $destRoot }
    New-Item -ItemType Directory -Force $destRoot | Out-Null
    if ($nativePkg) {
        Copy-Item -Recurse -Force "$nativePkg/addons" $destRoot
    }
    Write-Ok "assembled $((Resolve-Path $destRoot).Path)"
}

# --- Main ----------------------------------------------------------------
$winPkg = $null; $linPkg = $null
if ($Windows) { $winPkg = Build-Windows }
if ($Linux) { $linPkg = Build-Linux }

Write-Step "Dist"
if ($Windows) { Build-Dist $winPkg $DistWin }
if ($Linux) { Build-Dist $linPkg $DistLin }

Write-Step "Done"
if ($Windows) { Write-Ok "Windows -> dist/windows/" }
if ($Linux) { Write-Ok "Linux   -> dist/linux/" }
