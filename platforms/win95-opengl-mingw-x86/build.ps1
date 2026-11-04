# Copyright (c) 2026 Scintillae Ltd. All rights reserved.

<#
.SYNOPSIS
    Build relic for Windows 95/98/ME/XP / Legacy MinGW x86 (32-bit).

    This platform uses the original MinGW (not MinGW-w64) with GCC targeting
    Windows 95 and later for maximum backward compatibility.

.PARAMETER Config
    Release (default) or Debug.

.PARAMETER Clean
    Wipe the build directory first.

.PARAMETER MinGWPath
    Path to legacy MinGW installation (required). The toolchain is NOT
    auto-detected: pass the install root explicitly with -MinGWPath.

.EXAMPLE
    .\platforms\win95-opengl-mingw-x86\build.ps1 -MinGWPath "C:\Program Files (x86)\MinGW"
    .\platforms\win95-opengl-mingw-x86\build.ps1 -MinGWPath "C:\Program Files (x86)\MinGW" -Config Debug -Clean
#>
# Copyright (c) 2026 Scintillae Ltd. All rights reserved.

[CmdletBinding()]
param(
    [ValidateSet('Release','Debug','RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Clean,
    [Parameter(Mandatory=$true)][string]$MinGWPath,
    [switch]$ExperimentalServer
)

$ErrorActionPreference = 'Stop'

$PlatformId  = 'win95-opengl-mingw-x86'
$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot    = Resolve-Path (Join-Path $ScriptDir '..\..')
$BuildDir    = Join-Path $RepoRoot ("build\$PlatformId")
$BinDir      = Join-Path $RepoRoot ("bin\$PlatformId")

# ---------------------------------------------------------------------------
# Verify legacy MinGW
# ---------------------------------------------------------------------------
$gccPath = Join-Path $MinGWPath "bin\gcc.exe"
if (-not (Test-Path $gccPath)) {
    Write-Error "Legacy MinGW gcc not found at: $gccPath"
    Write-Host ""
    Write-Host "Pass the legacy MinGW install root with -MinGWPath:" -ForegroundColor Yellow
    Write-Host "  .\build.ps1 -MinGWPath 'C:\Program Files (x86)\MinGW'" -ForegroundColor Yellow
    exit 1
}

# Add MinGW and MSYS to PATH for this session
# Try to find MSYS relative to the passed -MinGWPath (could be a sibling
# directory or bundled inside the MinGW root - no global probing).
$msysPath = $null
$possibleMsys = @(
    (Join-Path $MinGWPath "..\MSYS"),
    (Join-Path $MinGWPath "..\msys"),
    (Join-Path $MinGWPath "MSYS"),
    (Join-Path $MinGWPath "msys")
)
foreach ($p in $possibleMsys) {
    if (Test-Path $p) {
        $msysPath = $p
        break
    }
}

if ($msysPath) {
    # MSYS 1.0 has tools in 1.0\bin subdirectory
    $msysBin = Join-Path $msysPath "1.0\bin"
    if (Test-Path $msysBin) {
        $env:PATH = "$MinGWPath\bin;$msysBin;$env:PATH"
    } else {
        $env:PATH = "$MinGWPath\bin;$msysPath\bin;$env:PATH"
    }
} else {
    $env:PATH = "$MinGWPath\bin;$env:PATH"
}

if ($Clean) {
    if (Test-Path $BuildDir) {
        Write-Host "Cleaning $BuildDir" -ForegroundColor Yellow
        Remove-Item -Recurse -Force $BuildDir
    }
    if (Test-Path $BinDir) {
        Write-Host "Cleaning $BinDir" -ForegroundColor Yellow
        Remove-Item -Recurse -Force $BinDir
    }
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
New-Item -ItemType Directory -Force -Path $BinDir | Out-Null

# ---------------------------------------------------------------------------
# Build using Makefile (legacy MinGW doesn't use CMake)
# ---------------------------------------------------------------------------
Write-Host "Building ($PlatformId / $Config)..." -ForegroundColor Cyan
Write-Host "Using legacy MinGW at: $MinGWPath" -ForegroundColor Cyan

# Find make.exe (only under the passed -MinGWPath / its sibling MSYS)
$makePath = $null
$possibleMakes = @(
    (Join-Path $MinGWPath "bin\make.exe"),
    (Join-Path $MinGWPath "msys\1.0\bin\make.exe"),
    (Join-Path $MinGWPath "msys\bin\make.exe")
)
if ($msysPath) {
    $possibleMakes = @(
        (Join-Path $msysPath "bin\make.exe"),
        (Join-Path $msysPath "1.0\bin\make.exe")
    ) + $possibleMakes
}
foreach ($p in $possibleMakes) {
    Write-Host "Checking: $p" -ForegroundColor DarkGray
    if (Test-Path $p) {
        $makePath = $p
        Write-Host "Found make at: $p" -ForegroundColor Green
        break
    }
}
if (-not $makePath) {
    Write-Error "make.exe not found. Please ensure MSYS is installed with make."
    Write-Host ""
    Write-Host "Searched in:" -ForegroundColor Yellow
    foreach ($p in $possibleMakes) {
        Write-Host "  $p" -ForegroundColor Yellow
    }
    exit 1
}
Write-Host "Using make: $makePath" -ForegroundColor Cyan

Push-Location $RepoRoot
try {
    # Serialise the make step across every concurrent win95 build. MSYS 1.0
    # keeps its mount/process table in shared memory, so two instances of
    # this legacy toolchain running at once flake out ("gcc: Command not
    # found" mid-build and similar). uber.ps1 builds the
    # win95 platforms in parallel, so they queue on a global
    # mutex here instead. Only the make calls need the lock; gcc/strip
    # in MinGW\bin are native binaries and are safe in parallel.
    $msysMutex = New-Object System.Threading.Mutex($false, 'Global\TigryxWin95MsysBuild')
    $msysLock = $false
    try {
        $msysLock = $msysMutex.WaitOne()
    } catch [System.Threading.AbandonedMutexException] {
        $msysLock = $true   # previous holder died; we own it now
    }
    if (-not $msysLock) { Write-Error "Could not acquire win95 MSYS build lock"; exit 1 }
    try {

    $mk = "platforms\win95-opengl-mingw-x86\Makefile"

    # Extra flags are APPENDED by the Makefile (EXTRA_CFLAGS/EXTRA_LDFLAGS).
    # Do not pass CFLAGS=/LDFLAGS= here: a command-line assignment overrides
    # every assignment in the Makefile, which silently drops RELIC_WIN95,
    # RELIC_PLATFORM_ID / RELIC_BACKEND_ID and -Os. The Makefile already
    # pins WINVER/_WIN32_WINNT to 0x0400 and the 4.0 subsystem version.
    $legacyCFlags = ""
    if ($ExperimentalServer -or $env:TIGRYX_EXPERIMENTAL_SERVER) {
        $legacyCFlags = "$legacyCFlags -DTIGRYX_EXPERIMENTAL_SERVER"
    }
    $legacyLdFlags = "-Wl,--subsystem=console -lwinmm"
    if ($ExperimentalServer -or $env:TIGRYX_EXPERIMENTAL_SERVER) {
        $legacyLdFlags = "$legacyLdFlags -lws2_32"
    }

    & $makePath "-f" $mk "PLATFORM_ID=$PlatformId" "EXTRA_CFLAGS=$legacyCFlags" "EXTRA_LDFLAGS=$legacyLdFlags" clean 2>$null
    & $makePath "-f" $mk "PLATFORM_ID=$PlatformId" "EXTRA_CFLAGS=$legacyCFlags" "EXTRA_LDFLAGS=$legacyLdFlags"
    $rc = $LASTEXITCODE
    if ($rc -ne 0) { Write-Error "Build failed ($rc)"; exit $rc }

    } finally {
        $msysMutex.ReleaseMutex()
    }
} finally {
    Pop-Location
}

# ---------------------------------------------------------------------------
# Copy binaries to bin directory
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "Verifying binaries in $BinDir..." -ForegroundColor Cyan

# The 'w' names are the GUI-subsystem twins (tigryxw/tigryxriw/tigryxvmw/
# tigryxppw). They exist so nothing Explorer launches - or spawns - is a
# console-subsystem image, which is the only reliable way to keep Win9x from
# creating a console window for it.
$builtBins = @('tigryx.exe', 'tigryxw.exe', 'tigryxri.exe', 'tigryxriw.exe',
               'tigryxvm.exe', 'tigryxvmw.exe', 'tigryxpp.exe', 'tigryxppw.exe')
foreach ($bin in $builtBins) {
    $binPath = Join-Path $BinDir $bin
    if (Test-Path $binPath) {
        Write-Host "  $bin"
    }
}

# ---------------------------------------------------------------------------
# Strip symbols to reduce size
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "Stripping symbols from binaries..." -ForegroundColor Cyan
$stripPath = Join-Path $MinGWPath "bin\strip.exe"
if (-not (Test-Path $stripPath)) { $stripPath = "strip" }

foreach ($bin in $builtBins) {
    $binPath = Join-Path $BinDir $bin
    if (Test-Path $binPath) {
        Write-Host "  $bin"
        & $stripPath $binPath 2>$null
    }
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "Build complete ($PlatformId / $Config)" -ForegroundColor Green
Write-Host ""
Write-Host "Binaries in ${BinDir}:" -ForegroundColor Green
foreach ($bin in $builtBins) {
    $binPath = Join-Path $BinDir $bin
    if (Test-Path $binPath) {
        $size = (Get-Item $binPath).Length
        Write-Host "  $bin ($size bytes)"
    }
}
Write-Host ""
Write-Host "Target: Windows 95/98/ME/XP+ (subsystem version 4.0)" -ForegroundColor Green
Write-Host "Try:   $BinDir\tigryx.exe run $RepoRoot\samples\cube.tp"

# Stage the install-root layout (fonts\ + artifact-working\) in the bin
# folder so binaries run from here resolve assets the same way an
# installed copy does.
& (Join-Path $RepoRoot 'scripts\stage-bin.ps1') -BinDir $BinDir
