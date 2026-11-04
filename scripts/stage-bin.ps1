# Copyright (c) 2026 Scintillae Ltd. All rights reserved.

# -----------------------------------------------------------------------------
# Stage a bin\<platform> directory in the installed Tigryx layout:
#
#   <BinDir>\           executables (produced by the platform build itself)
#   <BinDir>\fonts\     typefaces the backend scans at runtime
#   <BinDir>\artifact\  the importable library the preprocessor resolves
#                       #import against
#
# The artifact tree staged here is the shipped artifact\ subset (licensed
# gui + system), matching the installed layout. artifact-working\ is
# development-only: in a dev checkout the preprocessor resolves it first,
# so builds run from this bin\ still see the full library, and the
# installer builds pull platform trees (dos/win/...) from it directly -
# it is never copied into a bin\ or shipped payload.
#
# Fonts: DOS targets ship pre-baked .fnt caches (loading .ttf on a 486 is
# impractical); everything else ships the .ttf/.otf set. The backend scans
# <exe_dir>\fonts on every platform, so this folder layout matches what an
# installed copy looks like.
#
# Usage:
#   scripts\stage-bin.ps1 -BinDir bin\windows-opengl-mingw-x64
#   scripts\stage-bin.ps1 -BinDir bin\dos-relic3d-djgpp-i386 -Dos
# -----------------------------------------------------------------------------
param(
    [Parameter(Mandatory=$true)]
    [string]$BinDir,
    [switch]$Dos
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Split-Path -Parent $ScriptDir

if (-not (Test-Path $BinDir)) {
    throw "No such bin directory: $BinDir"
}

# --- fonts/ ---
# Copy only files whose content actually changed. Destination files can be
# locked while a program has them mapped (a VM guest reading this bin over
# a shared folder, a running tool that loaded the typeface, an AV scan) -
# overwriting an identical file is pointless and a hard failure there would
# fail the whole platform build for no benefit.
# Get-FileHash needs PowerShell 4+. On older hosts (PS2-era Win boxes this
# script runs on during win95-family builds) hash via .NET instead - same
# SHA256, just spelled out.
function Get-ContentHashSha256 {
    param([string]$Path)
    if (Get-Command Get-FileHash -ErrorAction SilentlyContinue) {
        return (Get-FileHash $Path -Algorithm SHA256).Hash
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [System.IO.File]::OpenRead($Path)
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($fs))).Replace("-", "")
    } finally {
        $fs.Close()
        $sha.Dispose()
    }
}

function Copy-IfChanged {
    param([string]$Source, [string]$DestDir)
    $name = Split-Path -Leaf $Source
    $dest = Join-Path $DestDir $name
    if (Test-Path $dest) {
        $sLen = (Get-Item $Source).Length
        $dLen = (Get-Item $dest).Length
        if ($sLen -eq $dLen -and
            (Get-ContentHashSha256 $Source) -eq
            (Get-ContentHashSha256 $dest)) {
            return   # already staged, identical content
        }
    }
    try {
        Copy-Item -Path $Source -Destination $DestDir -Force -ErrorAction Stop
    } catch {
        # Locked destination: if the bytes already match the source there is
        # nothing to do - warn and keep going rather than fail the build.
        if ((Test-Path $dest) -and
            ((Get-Item $Source).Length -eq (Get-Item $dest).Length) -and
            ((Get-ContentHashSha256 $Source) -eq
             (Get-ContentHashSha256 $dest))) {
            Write-Host "  locked but identical, keeping existing: $name" -ForegroundColor Yellow
            return
        }
        throw
    }
}

$FontsSrc  = Join-Path $RepoRoot "fonts"
$FontsDest = Join-Path $BinDir "fonts"
New-Item -ItemType Directory -Force -Path $FontsDest | Out-Null
if ($Dos) {
    # Pre-baked .fnt caches only - instant loads on DOS-class hardware.
    Get-ChildItem -Path "$FontsSrc\*.fnt" -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-IfChanged $_.FullName $FontsDest }
} else {
    Get-ChildItem -Path $FontsSrc -File |
        Where-Object { $_.Extension -in ".ttf", ".otf" } |
        ForEach-Object { Copy-IfChanged $_.FullName $FontsDest }
}

# Loose font files at the bin root are dead weight - the backend scans
# fonts\ (and assets\), not the root itself. Clear any staged by older
# builds so the layout is exactly the installed one.
Remove-Item -Path "$BinDir\*.ttf", "$BinDir\*.otf", "$BinDir\*.fnt" -Force -ErrorAction SilentlyContinue

# --- artifact/ (optional: staged only when a repo-root artifact\ exists ---
#  the source release does not ship the artifact library)
$ArtifactSrc  = Join-Path $RepoRoot "artifact"
$ArtifactDest = Join-Path $BinDir "artifact"
if (Test-Path $ArtifactDest) { Remove-Item $ArtifactDest -Recurse -Force }
if (Test-Path $ArtifactSrc) { Copy-Item $ArtifactSrc $ArtifactDest -Recurse }

# --- licence files (same set the installer payload carries) ---
& (Join-Path $ScriptDir 'stage-licenses.ps1') -BinDir $BinDir
