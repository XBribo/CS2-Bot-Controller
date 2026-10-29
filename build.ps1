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

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$Root = $PSScriptRoot
$DistWin = Join-Path $Root "dist/windows"
$DistLin = Join-Path $Root "dist/linux"
$DistCss = Join-Path $Root "dist/CounterStrikeSharp"
$DistSw2 = Join-Path $Root "dist/SwiftlyS2"

# Default to both platforms.
if (-not ($Windows -or $Linux)) {
    $Windows = $true; $Linux = $true
}

function Write-Step([string]$msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }
function Write-Ok([string]$msg) { Write-Host "  $msg" -ForegroundColor Green }

# Convert a Windows path to WSL.
function ConvertTo-WslPath([string]$p) {
    $full = (Resolve-Path -LiteralPath $p).Path
    $drive = $full.Substring(0, 1).ToLower()
    $rest = $full.Substring(2) -replace '\\', '/'
    return "/mnt/$drive$rest"
}

# --- Clean ---------------------------------------------------------------
# Remove build and package outputs.
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
# Build the Windows DLL.
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
# Build in WSL and stage the Linux package.
function Build-Linux {
    Write-Step "Linux (WSL: $WslDistro)"
    $srcWsl = ConvertTo-WslPath $Root
    $bash = @"
set -e
cmake -S '$srcWsl' -B ~/bc-build -DCMAKE_BUILD_TYPE=$Config
cmake --build ~/bc-build -j`$(nproc)
test -f ~/bc-build/package/addons/BotController/bin/linuxsteamrt64/BotController.so
echo "BUILD_OK"
"@
    $bash = $bash -replace "`r`n", "`n"
    wsl.exe -d $WslDistro -e bash -lc $bash | Out-Host
    if ($LASTEXITCODE) { throw "WSL linux build failed" }
    # Stage the WSL package locally.
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
# Reset the package output directory.
function Reset-DistRoot([string]$destRoot) {
    if (Test-Path $destRoot) { Remove-Item -Recurse -Force $destRoot }
    New-Item -ItemType Directory -Force $destRoot | Out-Null
}

# Assemble the platform's Metamod package.
function Build-Dist([string]$packageRoot, [string]$destRoot) {
    Reset-DistRoot $destRoot
    if ($packageRoot) {
        $dstBot = Join-Path $destRoot "addons/BotController"
        $dstMeta = Join-Path $destRoot "addons/metamod"
        New-Item -ItemType Directory -Force $dstBot | Out-Null
        New-Item -ItemType Directory -Force $dstMeta | Out-Null

        Copy-Item -Recurse -Force "$packageRoot/addons/BotController/bin" $dstBot
        Copy-Item -Force "$packageRoot/addons/BotController/gamedata.json" $dstBot
        Copy-Item -Force "$packageRoot/addons/BotController/config.json" $dstBot
        Copy-Item -Force "$packageRoot/addons/metamod/BotController.vdf" $dstMeta
    }
    Write-Ok "assembled $((Resolve-Path $destRoot).Path)"
}

# --- C# plugins ----------------------------------------------------------
# Build the CounterStrikeSharp provider and API.
function Build-CSharp {
    Write-Step "C# (shared API + provider plugin)"
    $impl = Join-Path $Root "csharp/BotControllerImpl"
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

# Package CounterStrikeSharp assemblies.
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
# Build the SwiftlyS2 provider.
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

Write-Step "Dist (Metamod)"
if ($Windows) { Build-Dist $winPkg $DistWin }
if ($Linux) { Build-Dist $linPkg $DistLin }

# Build and package both managed providers.
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
