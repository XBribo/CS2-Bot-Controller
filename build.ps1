#requires -Version 7.0
[CmdletBinding()]
param(
    [switch]$Windows,
    [switch]$Linux,
    [switch]$Clean,
    [switch]$SkipCsharp,
    [string]$Config = "Release",
    [string]$WslDistro = "Ubuntu-24.04",
    [string]$Generator = "Visual Studio 18 2026"
)

$ErrorActionPreference = "Stop"

# Force UTF-8 so CMake/MSBuild Chinese output renders correctly
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$Root = $PSScriptRoot
$DistWin = Join-Path $Root "dist/windows"
$DistLin = Join-Path $Root "dist/linux"
$DistCss = Join-Path $Root "dist/CounterStrikeSharp"
$DistSw2 = Join-Path $Root "dist/SwiftlyS2"

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
# Recreate a dist root so each artifact family is packaged independently.
function Reset-DistRoot([string]$destRoot) {
    if (Test-Path $destRoot) { Remove-Item -Recurse -Force $destRoot }
    New-Item -ItemType Directory -Force $destRoot | Out-Null
}

# Copy only the expected native package files into a per-platform dist folder.
function Build-Dist([string]$nativePkg, [string]$destRoot) {
    Reset-DistRoot $destRoot
    if ($nativePkg) {
        $dstBot = Join-Path $destRoot "addons/BotController"
        $dstMeta = Join-Path $destRoot "addons/metamod"
        New-Item -ItemType Directory -Force $dstBot | Out-Null
        New-Item -ItemType Directory -Force $dstMeta | Out-Null

        Copy-Item -Recurse -Force "$nativePkg/addons/BotController/bin" $dstBot
        Copy-Item -Force "$nativePkg/addons/BotController/gamedata.json" $dstBot
        Copy-Item -Force "$nativePkg/addons/metamod/BotController.vdf" $dstMeta
    }
    Write-Ok "assembled $((Resolve-Path $destRoot).Path)"
}

# --- C# plugins ----------------------------------------------------------
# Build the shared API DLL + the BotControllerImpl provider plugin.
function Build-CSharp {
    Write-Step "C# (shared API + provider plugin)"
    $impl = Join-Path $Root "csharp/BotControllerImpl"
    # Building the provider also builds the referenced shared API project.
    dotnet build $impl -c $Config | Out-Host
    if ($LASTEXITCODE) { throw "dotnet build (csharp) failed" }
    $sharedDll = Join-Path $Root "csharp/BotControllerApi/bin/$Config/BotControllerApi.dll"
    $pluginDll = Join-Path $impl "bin/$Config/BotControllerImpl.dll"
    foreach ($d in @($sharedDll, $pluginDll)) {
        if (-not (Test-Path $d)) { throw "csharp build produced no $(Split-Path $d -Leaf)" }
    }
    Write-Ok "BotControllerApi.dll + BotControllerImpl.dll built"
    return @{ Shared = $sharedDll; Plugin = $pluginDll }
}

# Deploy managed DLLs into a dist tree's CounterStrikeSharp layout.
function Deploy-CSharp([hashtable]$csOut, [string]$destRoot) {
    if (-not $csOut) { return }
    Reset-DistRoot $destRoot
    $css = Join-Path $destRoot "addons/counterstrikesharp"
    $sharedDir = Join-Path $css "shared/BotControllerApi"
    $pluginDir = Join-Path $css "plugins/BotControllerImpl"
    New-Item -ItemType Directory -Force $sharedDir | Out-Null
    New-Item -ItemType Directory -Force $pluginDir | Out-Null
    Copy-Item -Force $csOut.Shared $sharedDir
    Copy-Item -Force $csOut.Plugin $pluginDir
    Write-Ok "deployed C# into $((Resolve-Path $css).Path)"
}

# --- SwiftlyS2 plugin ----------------------------------------------------
# Build the BotControllerImplSW2 plugin for SwiftlyS2.
function Build-CSharpSW2 {
    Write-Step "C# SwiftlyS2 (BotControllerImplSW2)"
    $sw2 = Join-Path $Root "csharp/BotControllerImplSW2"
    dotnet build $sw2 -c $Config | Out-Host
    if ($LASTEXITCODE) { throw "dotnet build (swiftlys2) failed" }
    $buildDir = Join-Path $sw2 "build"
    $pluginDll = Join-Path $buildDir "BotControllerImplSW2.dll"
    if (-not (Test-Path $pluginDll)) { throw "swiftlys2 build produced no BotControllerImplSW2.dll" }
    $dlls = Get-ChildItem -Path $buildDir -Filter "*.dll" | Select-Object -ExpandProperty FullName
    Write-Ok "BotControllerImplSW2.dll built ($($dlls.Count) assemblies in output)"
    return @{ BuildDir = $buildDir; Dlls = $dlls }
}

# Deploy SwiftlyS2 managed DLLs into a dist tree's swiftlys2 layout.
function Deploy-CSharpSW2([hashtable]$sw2Out, [string]$destRoot) {
    if (-not $sw2Out) { return }
    Reset-DistRoot $destRoot
    $sw2Dir = Join-Path $destRoot "addons/swiftlys2/plugins/BotControllerImplSW2"
    New-Item -ItemType Directory -Force $sw2Dir | Out-Null
    Copy-Item -Recurse -Force -Path "$($sw2Out.BuildDir)/*" -Destination $sw2Dir
    Write-Ok "deployed SwiftlyS2 into $((Resolve-Path $sw2Dir).Path)"
}

# --- Main ----------------------------------------------------------------
$winPkg = $null; $linPkg = $null
if ($Windows) { $winPkg = Build-Windows }
if ($Linux) { $linPkg = Build-Linux }

Write-Step "Dist (Native)"
if ($Windows) { Build-Dist $winPkg $DistWin }
if ($Linux) { Build-Dist $linPkg $DistLin }

# C# managed plugins: build once, deploy into each assembled dist tree
$csOut = $null; $sw2Out = $null
if (-not $SkipCsharp) {
    $csOut = Build-CSharp
    $sw2Out = Build-CSharpSW2
}
if ($csOut) {
    Write-Step "Deploy C# (CounterStrikeSharp)"
    Deploy-CSharp $csOut $DistCss
}
if ($sw2Out) {
    Write-Step "Deploy C# (SwiftlyS2)"
    Deploy-CSharpSW2 $sw2Out $DistSw2
}

Write-Step "Done"
if ($Windows) { Write-Ok "Windows -> dist/windows/" }
if ($Linux) { Write-Ok "Linux   -> dist/linux/" }
if ($csOut) { Write-Ok "CounterStrikeSharp -> dist/CounterStrikeSharp/" }
if ($sw2Out) { Write-Ok "SwiftlyS2         -> dist/SwiftlyS2/" }
