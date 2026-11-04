# Copyright (c) 2026 Scintillae Ltd. All rights reserved.

<#
.SYNOPSIS
    Build relic for DOS / DJGPP 386 (32-bit, 386-compatible) via CMake.

    This platform uses DJGPP (GCC for DOS) targeting 386 (32-bit) DOS.
    Compatible with 386 and later processors, and is the target for
    machines with NO FPU (386SX, 486SX, and similar): SX-class chips have no
    x87, so any float op not covered below traps into DJGPP's -lemu
    emulator, which reflects through the DPMI host and costs on the order
    of 25,000 cycles per operation -- roughly 10-25x a normal FPU-emulation
    trap, because of that extra reflection. This build avoids the FPU
    entirely rather than emulating it faster, via two independent flags:

      -DTIGRYX_SOFTFP (CMake option, this script's main lever): compiles
      tigryx_core/tigryx_backend/tigryx_jit with -msoft-float and links in the
      hand-written f32 runtime from src/softfp/ (see CMakeLists.txt's
      option() comment for what that covers and why it must apply to the
      whole dependency graph together). This is what actually removes the
      FPU from the picture: -msoft-float lowers every float op to a plain
      call, so nothing ever traps.

      -DRELIC_FIXED_POINT_3D (compile define, still applied below): on top
      of soft-float, replaces the software rasteriser's hot sub-span
      perspective UV loop with 64-bit-integer fixed-point, which is still
      faster than even a soft-float divide there. All other 3D stages
      (camera setup, matrix math, vertex transform, clipping) stay
      soft-float rather than also being hand-converted to fixed-point.

.PARAMETER Config
    Release (default) or Debug.

.PARAMETER Clean
    Wipe the build directory first.

.PARAMETER DJGPPPath
    Path to DJGPP installation (required). The toolchain is NOT
    auto-detected: pass the install root explicitly with -DJGPPPath.

.PARAMETER DosNet
    Enable the lwIP TCP/IP stack for HTTP support. Built from source
    (src\dosnet + thirdparty\lwip) - no prebuilt library needed.
    When omitted (default), @http_* functions return -1 (not supported).

.EXAMPLE
    .\platforms\dos-frelic3d-djgpp-i386\build.ps1 -DJGPPPath "C:\Program Files\djgpp-mingw-gcc1220-standalone"
    .\platforms\dos-frelic3d-djgpp-i386\build.ps1 -DJGPPPath "C:\Program Files\djgpp-mingw-gcc1220-standalone" -Config Debug -Clean
#>
# Copyright (c) 2026 Scintillae Ltd. All rights reserved.

[CmdletBinding()]
param(
    [ValidateSet('Release','Debug','RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Clean,
    [Parameter(Mandatory=$true)][string]$DJGPPPath,
    [switch]$DosNet,
    [switch]$Profile
)

$ErrorActionPreference = 'Stop'

$PlatformId  = 'dos-frelic3d-djgpp-i386'
$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot    = Resolve-Path (Join-Path $ScriptDir '..\..')
$BuildDir    = Join-Path $RepoRoot ("build\$PlatformId")
$BinDir      = Join-Path $RepoRoot ("bin\$PlatformId")

# ---------------------------------------------------------------------------
# DJGPP toolchain root (required - no auto-detection)
# ---------------------------------------------------------------------------
$djgppPath = $DJGPPPath
if (-not (Test-Path $djgppPath)) {
    Write-Error "DJGPP directory not found at: $djgppPath"
    Write-Host ""
    Write-Host "Pass the DJGPP install root with -DJGPPPath:" -ForegroundColor Yellow
    Write-Host "  .\build.ps1 -DJGPPPath 'C:\Program Files\djgpp-mingw-gcc1220-standalone'" -ForegroundColor Yellow
    Write-Host ""
    exit 1
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

# Determine the correct bin directory
$djgppBinDir = "$djgppPath\i586-pc-msdosdjgpp\bin"
if (-not (Test-Path $djgppBinDir)) {
    $djgppBinDir = "$djgppPath\bin"
}

# Set DJGPP environment variables (based on setenv.bat)
$env:PATH = "$djgppBinDir;$djgppPath\bin;$env:PATH"
Remove-Item env:DJGPP -ErrorAction SilentlyContinue
Remove-Item env:COMPILER_PATH -ErrorAction SilentlyContinue
Remove-Item env:C_INCLUDE_PATH -ErrorAction SilentlyContinue
Remove-Item env:CPLUS_INCLUDE_PATH -ErrorAction SilentlyContinue
$env:GCC_EXEC_PREFIX = "$djgppPath\lib\gcc\"
if ($djgppPath -match "i586-pc-msdosdjgpp$") { $djgppPath = Split-Path -Parent $djgppPath }
$env:DJDIR = "$djgppPath\i586-pc-msdosdjgpp"
$env:LIBRARY_PATH = "$djgppPath\i586-pc-msdosdjgpp\lib"
$djgppLibDir = "$djgppPath\i586-pc-msdosdjgpp\lib"

# Configure + build
# Try Ninja first, fall back to Makefiles

# Base flags — profile mode adds -pg and removes only the two flags that
# conflict with gprof (-fomit-frame-pointer kills the call-graph unwind,
# --strip-all removes the symbol table gprof reads). Everything else stays.
$cFlagsBase   = '-O3 -fomit-frame-pointer -march=i486 -w -ffunction-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-common -DRELIC_FIXED_POINT_3D'
$cxxFlagsBase = '-O3 -fomit-frame-pointer -march=i486 -w -ffunction-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-common -fno-exceptions -fno-rtti'
$linkFlagsBase = '-Wl,--strip-all -Wl,--gc-sections -Wl,--as-needed -Wl,--sort-common -Wl,--discard-all -lemu'

if ($Profile) {
    $cFlagsBase   = $cFlagsBase   -replace '-fomit-frame-pointer', '-pg'
    $cFlagsBase   = $cFlagsBase   -replace '-O3', '-O1'
    $cxxFlagsBase = $cxxFlagsBase -replace '-fomit-frame-pointer', '-pg'
    $cxxFlagsBase = $cxxFlagsBase -replace '-O3', '-O1'
    $linkFlagsBase = $linkFlagsBase -replace '-Wl,--strip-all ', ''
    Write-Host "Profiling ENABLED (-O1 -pg, no strip, frame pointers retained)" -ForegroundColor Yellow
}

$cmakeConfig = "cmake -S `"$RepoRoot`" -B `"$BuildDir`" -G `"Ninja`" " +
    "-DCMAKE_BUILD_TYPE=$Config " +
    "-DTIGRYX_BACKEND=allegro-fixed-dos " +
    "-DTIGRYX_JIT=x86-dos " +
    "-DRELIC_PLATFORM_ID=$PlatformId " +
    "-DTIGRYX_DOSNET=$(if ($DosNet) {'ON'} else {'OFF'}) " +
    "-DTIGRYX_SOFTFP=ON " +
    "-DTIGRYX_PROFILE=$(if ($Profile) {'ON'} else {'OFF'}) " +
    "-DCMAKE_C_COMPILER=`"$djgppBinDir\gcc.exe`" " +
    "-DCMAKE_SYSTEM_NAME=DOS " +
    "-DCMAKE_SYSTEM_VERSION=1 " +
    "-DCMAKE_CROSSCOMPILING=TRUE " +
    "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY " +
    "-DCMAKE_C_FLAGS=`"$cFlagsBase`" " +
    "-DCMAKE_EXE_LINKER_FLAGS=`"$linkFlagsBase`" "

Write-Host "Configuring + building ($PlatformId / $Config, soft-float + fixed-point 3D)..." -ForegroundColor Cyan
Write-Host "Using DJGPP at: $djgppPath" -ForegroundColor Cyan
if ($DosNet) {
    Write-Host "TCP/IP (lwIP): ENABLED (HTTP support)" -ForegroundColor Green
} else {
    Write-Host "TCP/IP (lwIP): disabled (no HTTP)" -ForegroundColor DarkGray
}

Invoke-Expression $cmakeConfig
$rc = $LASTEXITCODE
if ($rc -ne 0) { Write-Error "CMake configuration failed ($rc)"; exit $rc }

# Build everything except tigryxrc (CMakeLists.txt incorrectly includes
# tigryxce_main.c + vm.c in tigryxrc, causing duplicate main & undefined refs)
# Ensure JIT library is built before executables that link it
$cmakeTargets = @('tigryx_jit','tigryx','tigryxri','tigryxvm','tigryxpp')
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
foreach ($bin in @('tigryx.exe', 'tigryxri.exe', 'tigryxvm.exe', 'tigryxpp.exe')) {
    $binPath = Join-Path $BinDir $bin
    if (Test-Path $binPath) {
        Write-Host "  $bin"
        & "$djgppBinDir\strip.exe" "$binPath"
    }
}

# Copy CWSDPMI.EXE to bin folder (required for DOS DPMI support)
$cwsSource = Join-Path $RepoRoot 'tools\dos\CWSDPMI.EXE'
if (Test-Path $cwsSource) {
    Copy-Item $cwsSource $BinDir -Force
    Write-Host "Copied CWSDPMI.EXE to $BinDir" -ForegroundColor DarkGray
}

$relic = Join-Path $BinDir 'tigryx.exe'
Write-Host ""
$toolNote = ''
Write-Host "Built: $relic (32-bit DOS, soft-float + fixed-point 3D, + tigryxri, tigryxvm, tigryxpp$toolNote)" -ForegroundColor Green
Write-Host "Target: DOS (DJGPP GCC, 486-optimised, no FPU required)" -ForegroundColor Green
Write-Host "Try:   $relic run $RepoRoot\samples\cube.tp"

# Stage the install-root layout (fonts\ + artifact-working\) in the bin
# folder so binaries run from here resolve assets the same way an
# installed copy does.
& (Join-Path $RepoRoot 'scripts\stage-bin.ps1') -BinDir $BinDir -Dos
