# Copyright (c) 2026 Scintillae Ltd. All rights reserved.

<#
.SYNOPSIS
    Build relic for Windows 2000/XP+ / MinGW-w64 x86 (32-bit) via CMake.

    This platform uses MinGW-w64 with GCC for maximum Windows 2000/XP compatibility
    while supporting C99/C11 features that MSVC 2012 lacks.

.PARAMETER Config
    Release (default) or Debug.

.PARAMETER Backend
    Which graphics backend to link: opengl-win32 (default) or win32 (GDI).

.PARAMETER Clean
    Wipe the build directory first.

.PARAMETER MinGWPath
    Path to MinGW-w64 installation (required). The toolchain is NOT
    auto-detected: pass the install root explicitly with -MinGWPath.

.PARAMETER WindowsVersion
    Target Windows version: 5.0 (Windows 2000), 5.1 (Windows XP), or 6.0 (Windows Vista+).
    Default is 5.0 for maximum compatibility.

.EXAMPLE
    .\platforms\windows-opengl-mingw-x86\build.ps1 -MinGWPath "C:\Program Files\msys2\mingw32"
    .\platforms\windows-opengl-mingw-x86\build.ps1 -MinGWPath "C:\Program Files\msys2\mingw32" -Backend opengl-win32
    .\platforms\windows-opengl-mingw-x86\build.ps1 -MinGWPath "C:\Program Files\msys2\mingw32" -Config Debug -Clean
    .\platforms\windows-opengl-mingw-x86\build.ps1 -MinGWPath "C:\Program Files\msys2\mingw32" -WindowsVersion 5.1
#>
# Copyright (c) 2026 Scintillae Ltd. All rights reserved.

[CmdletBinding()]
param(
    [ValidateSet('Release','Debug','RelWithDebInfo')]
    [string]$Config = 'Release',
    [ValidateSet('opengl-win32','win32')]
    [string]$Backend = 'opengl-win32',
    [switch]$Clean,
    [Parameter(Mandatory=$true)][string]$MinGWPath,
    [ValidateSet('5.0','5.1','6.0')]
    [string]$WindowsVersion = '5.1',
    [switch]$ExperimentalServer,
    [switch]$ExperimentalMvm,
    # Extension toolchain build: a second pass alongside the standard output
    # with the experimental multi-VM host + JIT-linked runtime archives
    # (implies -ExperimentalMvm). Everything lands in <id>-ext build/bin dirs
    # so the standard binaries are untouched. install\build.ps1 uses this
    # toolchain to compile TIDE (and TigOS); the -ext bin is never shipped.
    [switch]$Ext
)

$ErrorActionPreference = 'Stop'

$PlatformId  = 'windows-opengl-mingw-x86'
if ($Ext) {
    $PlatformId      = "$PlatformId-ext"
    $ExperimentalMvm = $true
}
$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot    = Resolve-Path (Join-Path $ScriptDir '..\..')
$BuildDir    = Join-Path $RepoRoot ("build\$PlatformId")
$BinDir      = Join-Path $RepoRoot ("bin\$PlatformId")

# ---------------------------------------------------------------------------
# MinGW-w64 x86 toolchain root (required - no auto-detection)
# ---------------------------------------------------------------------------
$mingwPath = $MinGWPath
if (-not (Test-Path $mingwPath)) {
    Write-Error "MinGW-w64 directory not found at: $mingwPath"
    Write-Host ""
    Write-Host "Pass the MinGW-w64 32-bit install root with -MinGWPath:" -ForegroundColor Yellow
    Write-Host "  .\build.ps1 -MinGWPath 'C:\Program Files\msys2\mingw32'" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "For MSYS2: Install from https://www.msys2.org/ then run in MINGW32 shell:" -ForegroundColor Yellow
    Write-Host "  pacman -S mingw-w64-i686-gcc" -ForegroundColor Yellow
    exit 1
}
if (-not (Test-Path "$mingwPath\bin\gcc.exe")) {
    Write-Error "gcc.exe not found under $mingwPath\bin - is this a MinGW-w64 install root?"
    Write-Host "Pass the MinGW-w64 32-bit install root with -MinGWPath." -ForegroundColor Yellow
    exit 1
}

if ($Clean) {
    if (Test-Path $BuildDir) {
        Write-Host "Cleaning $BuildDir" -ForegroundColor Yellow
        try { Remove-Item -Recurse -Force $BuildDir -ErrorAction Stop }
        catch { Write-Host "  Warning: could not fully clean $BuildDir (files may be locked)" -ForegroundColor Yellow }
    }
    if (Test-Path $BinDir) {
        Write-Host "Cleaning $BinDir" -ForegroundColor Yellow
        try { Remove-Item -Recurse -Force $BinDir -ErrorAction Stop }
        catch { Write-Host "  Warning: could not fully clean $BinDir (files may be locked)" -ForegroundColor Yellow }
    }
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
New-Item -ItemType Directory -Force -Path $BinDir | Out-Null

# Windows version compatibility settings
$winVersionMap = @{
    '5.0' = '0x0500'  # Windows 2000
    '5.1' = '0x0501'  # Windows XP
    '6.0' = '0x0600'  # Windows Vista
}
$winVersionHex = $winVersionMap[$WindowsVersion]

# Subsystem version for PE header (required for Windows XP compatibility)
$subsystemFlags = "-Wl,--major-subsystem-version,$($WindowsVersion.Split('.')[0]) -Wl,--minor-subsystem-version,$($WindowsVersion.Split('.')[1])"

# Add MinGW to PATH for this session
$env:PATH = "$mingwPath\bin;$env:PATH"

# Configure + build
# Try Ninja first, fall back to MinGW Makefiles
$cmakeConfig = "cmake -S `"$RepoRoot`" -B `"$BuildDir`" -G `"Ninja`" " +
    "-DCMAKE_BUILD_TYPE=$Config " +
    "-DTIGRYX_BACKEND=$Backend " +
    "-DTIGRYX_JIT=x86-win " +
    "-DRELIC_PLATFORM_ID=$PlatformId " +
    "-DCMAKE_SYSTEM_VERSION=$WindowsVersion " +
    "-D_WIN32_WINNT=$winVersionHex " +
    "-DWINVER=$winVersionHex " +
    "-DCMAKE_C_COMPILER=`"$mingwPath\bin\gcc.exe`" " +
    "-DCMAKE_CXX_COMPILER=`"$mingwPath\bin\g++.exe`" " +
    "-DCMAKE_C_FLAGS=`"-Os`" " +
    "-DCMAKE_CXX_FLAGS=`"-Os`" " +
    "-DCMAKE_EXE_LINKER_FLAGS=`"-static -Wl,--strip-all $subsystemFlags`""

Write-Host "Configuring + building ($PlatformId / $Config, backend: $Backend, Windows: $WindowsVersion)..." -ForegroundColor Cyan
Write-Host "Using MinGW-w64 at: $mingwPath" -ForegroundColor Cyan

# NSNF experimental server (flag or env var via uber.ps1)
if ($ExperimentalServer -or $env:TIGRYX_EXPERIMENTAL_SERVER) {
    $cmakeConfig += " -DTIGRYX_EXPERIMENTAL_SERVER=ON"
}
if ($ExperimentalMvm -or $env:TIGRYX_EXPERIMENTAL_MVM) {
    $cmakeConfig += " -DTIGRYX_EXPERIMENTAL_MVM=ON"
}

Invoke-Expression $cmakeConfig
$rc = $LASTEXITCODE
if ($rc -ne 0) { Write-Error "CMake configuration failed ($rc)"; exit $rc }

# Build everything. Ensure JIT library is built before executables that link
# it. tigryxrc is retired (TIGRYX_BUILD_RC=OFF in CMake).
$cmakeTargets = @('tigryx_jit','tigryx','tigryxw','tigryxri','tigryxpp')
foreach ($t in $cmakeTargets) {
    Write-Host "Building target: $t ..." -ForegroundColor Cyan
    $cmd = "cmake --build `"$BuildDir`" --target $t -j"
    Invoke-Expression $cmd
    $rc = $LASTEXITCODE
    if ($rc -ne 0) { Write-Error "Build of $t failed ($rc)"; exit $rc }
}

# Strip symbols to reduce size
Write-Host ""
Write-Host "Stripping symbols from binaries..." -ForegroundColor Cyan
foreach ($bin in @('tigryx.exe', 'tigryxw.exe', 'tigryxri.exe', 'tigryxpp.exe')) {
    $binPath = Join-Path $BinDir $bin
    if (Test-Path $binPath) {
        Write-Host "  $bin"
        & "$mingwPath\bin\strip.exe" "$binPath"
    }
}

$relic = Join-Path $BinDir 'tigryx.exe'
Write-Host ""
Write-Host "Built: $relic (32-bit, + tigryxw, tigryxri, tigryxpp)" -ForegroundColor Green
Write-Host "Target: Windows $WindowsVersion+ (MinGW-w64 GCC)" -ForegroundColor Green
Write-Host "Try:   $relic run $RepoRoot\samples\cube.tp"

# Stage the install-root layout (fonts\ + artifact-working\) in the bin
# folder so binaries run from here resolve assets the same way an
# installed copy does.
& (Join-Path $RepoRoot 'scripts\stage-bin.ps1') -BinDir $BinDir
