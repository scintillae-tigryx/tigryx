# Copyright (c) 2026 Scintillae Ltd. All rights reserved.

<#
.SYNOPSIS
    Build tigryx for Windows / MinGW-w64 x64.

    Builds: tigryx, tigryxw, tigryxri, tigryxpp (JIT x64-win).
    Uses CMake for the main targets.

    Prerequisites:
      - MinGW-w64 x64 gcc

.PARAMETER Config
    Release (default) or Debug.

.PARAMETER Backend
    Which graphics backend to link: opengl-win32 (default) or win32 (GDI).

.PARAMETER Clean
    Wipe the build directory first.

.PARAMETER MinGWPath
    Path to MinGW-w64 x64 installation (required). The toolchain is NOT
    auto-detected: pass the install root explicitly with -MinGWPath.

.PARAMETER Ext
    Extension toolchain build: the standard toolset plus the
    experimental multi-VM host + JIT-linked runtime (implies -ExperimentalMvm),
    staged in bin\...-ext so standard output is untouched. Used by
    install\build.ps1 to compile TIDE/TigOS.

.EXAMPLE
    .\platforms\windows-opengl-mingw-x64\build.ps1 -MinGWPath "C:\Program Files\msys2\mingw64"
    .\platforms\windows-opengl-mingw-x64\build.ps1 -MinGWPath "C:\Program Files\msys2\mingw64" -Backend opengl-win32
    .\platforms\windows-opengl-mingw-x64\build.ps1 -MinGWPath "C:\Program Files\msys2\mingw64" -Config Debug -Clean
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

$PlatformId  = 'windows-opengl-mingw-x64'
$JitName     = 'x64-win'
if ($Ext) {
    $PlatformId      = "$PlatformId-ext"
    $ExperimentalMvm = $true
}
$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot    = Resolve-Path (Join-Path $ScriptDir '..\..')
$BuildDir    = Join-Path $RepoRoot ("build\$PlatformId")
$BinDir      = Join-Path $RepoRoot ("bin\$PlatformId")

# ---------------------------------------------------------------------------
# MinGW-w64 x64 toolchain root (required - no auto-detection)
# ---------------------------------------------------------------------------
$mingwPath = $MinGWPath
if (-not (Test-Path $mingwPath)) {
    Write-Error "MinGW-w64 directory not found at: $mingwPath"
    Write-Host ""
    Write-Host "Pass the MinGW-w64 x64 install root with -MinGWPath:" -ForegroundColor Yellow
    Write-Host "  .\build.ps1 -MinGWPath 'C:\Program Files\msys2\mingw64'" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "For MSYS2: Install from https://www.msys2.org/ then run in MINGW64 shell:" -ForegroundColor Yellow
    Write-Host "  pacman -S mingw-w64-x86_64-gcc" -ForegroundColor Yellow
    exit 1
}
if (-not (Test-Path "$mingwPath\bin\gcc.exe")) {
    Write-Error "gcc.exe not found under $mingwPath\bin - is this a MinGW-w64 install root?"
    Write-Host "Pass the MinGW-w64 x64 install root with -MinGWPath." -ForegroundColor Yellow
    exit 1
}

# Verify it's actually x64
$dumpmachine = & "$mingwPath\bin\gcc.exe" -dumpmachine 2>$null
if ($dumpmachine -and $dumpmachine -match "i686|i386") {
    Write-Warning "Found 32-bit MinGW at $mingwPath. For x64 builds, use mingw64."
}

if ($Clean) {
    if (Test-Path $BuildDir) {
        Write-Host "Cleaning $BuildDir" -ForegroundColor Yellow
        try { Remove-Item -Recurse -Force $BuildDir -ErrorAction Stop }
        catch { Write-Host "  Warning: could not fully clean $BuildDir (files may be locked): $($_.Exception.Message)" -ForegroundColor Yellow }
    }
    if (Test-Path $BinDir) {
        Write-Host "Cleaning $BinDir" -ForegroundColor Yellow
        try { Remove-Item -Recurse -Force $BinDir -ErrorAction Stop }
        catch { Write-Host "  Warning: could not fully clean $BinDir (files may be locked): $($_.Exception.Message)" -ForegroundColor Yellow }
    }
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
New-Item -ItemType Directory -Force -Path $BinDir   | Out-Null

# Add MinGW to PATH for this session
$env:PATH = "$mingwPath\bin;$env:PATH"

# ---------------------------------------------------------------------------
# Choose CMake generator
# ---------------------------------------------------------------------------
$ninja = Get-Command ninja -ErrorAction SilentlyContinue
if ($ninja) {
    $Generator = 'Ninja'
} else {
    $Generator = 'MinGW Makefiles'
}

# ---------------------------------------------------------------------------
# CMake configure + build (tigryx, tigryxw, tigryxri, tigryxpp)
# ---------------------------------------------------------------------------
$cmakeConfig = "cmake -S `"$RepoRoot`" -B `"$BuildDir`" -G `"$Generator`" " +
    "-DCMAKE_BUILD_TYPE=$Config " +
    "-DTIGRYX_BACKEND=$Backend " +
    "-DRELIC_PLATFORM_ID=$PlatformId " +
    "-DTIGRYX_JIT=$JitName " +
    "-DCMAKE_C_COMPILER=`"$mingwPath\bin\gcc.exe`" " +
    "-DCMAKE_C_FLAGS=`"-Wall -O2`" " +
    "-DCMAKE_EXE_LINKER_FLAGS=`"-static -Wl,--gc-sections -Wl,--strip-all`""

Write-Host "Configuring + building ($PlatformId / $Config, backend: $Backend)..." -ForegroundColor Cyan
Write-Host "Using MinGW-w64 at: $mingwPath" -ForegroundColor Cyan
Write-Host "CMake generator: $Generator" -ForegroundColor Cyan

# NSNF experimental server (flag or env var via uber.ps1)
if ($ExperimentalServer -or $env:TIGRYX_EXPERIMENTAL_SERVER) {
    $cmakeConfig += " -DTIGRYX_EXPERIMENTAL_SERVER=ON"
}
# experimental multi-VM host (flag or env var via uber.ps1)
if ($ExperimentalMvm -or $env:TIGRYX_EXPERIMENTAL_MVM) {
    $cmakeConfig += " -DTIGRYX_EXPERIMENTAL_MVM=ON"
}

Invoke-Expression $cmakeConfig
$rc = $LASTEXITCODE
if ($rc -ne 0) { Write-Error "CMake configuration failed ($rc)"; exit $rc }

# Build everything. tigryxrc is retired (TIGRYX_BUILD_RC=OFF in CMake).
$cmakeTargets = @('tigryx','tigryxw','tigryxri','tigryxpp')
foreach ($t in $cmakeTargets) {
    Write-Host "Building target: $t ..." -ForegroundColor Cyan
    $cmd = "cmake --build `"$BuildDir`" --target $t -j"
    Invoke-Expression $cmd
    $rc = $LASTEXITCODE
    if ($rc -ne 0) { Write-Error "Build of $t failed ($rc)"; exit $rc }
}

# ---------------------------------------------------------------------------
# Strip symbols to reduce size
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "Stripping symbols from binaries..." -ForegroundColor Cyan
foreach ($bin in @('tigryx.exe', 'tigryxw.exe', 'tigryxri.exe', 'tigryxpp.exe')) {
    $binPath = Join-Path $BinDir $bin
    if (Test-Path $binPath) {
        Write-Host "  $bin"
        & "$mingwPath\bin\strip.exe" "$binPath"
    }
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "Build complete ($PlatformId / $Config, backend: $Backend)" -ForegroundColor Green
Write-Host ""
Write-Host "Binaries in ${BinDir}:" -ForegroundColor Green
foreach ($bin in @('tigryx.exe', 'tigryxw.exe', 'tigryxri.exe', 'tigryxpp.exe')) {
    $binPath = Join-Path $BinDir $bin
    if (Test-Path $binPath) {
        $size = (Get-Item $binPath).Length
        Write-Host "  $bin ($size bytes)"
    }
}
Write-Host ""
Write-Host "Try:   $BinDir\tigryx.exe run $RepoRoot\samples\cube.tp"

# Stage the install-root layout (fonts\ + artifact-working\) in the bin
# folder so binaries run from here resolve assets the same way an
# installed copy does.
& (Join-Path $RepoRoot 'scripts\stage-bin.ps1') -BinDir $BinDir
