# Copyright (c) 2026 Scintillae Ltd. All rights reserved.

# -----------------------------------------------------------------------------
# stage-licenses.ps1 - copy the licence files into a bin\<platform> directory.
#
# Source-release variant: stages only the GPL-edition files present in this
# tree (COPYING, EXCEPTION.md, COMMERCIAL.md, LICENSING). Commercial-edition
# licence files (LICENSE.OTR, TIDE-LICENSE.md) are not part of the source
# release and are never staged here.
#
# DOS bins (dos-* native builds and the legacy DR*/DT*/DG* dirs) get 8.3
# names for the two that don't fit: EXCEPT.TXT, COMMERCL.TXT.
#
# Usage:
#   scripts\stage-licenses.ps1 -BinDir bin\windows-opengl-mingw-x64
# -----------------------------------------------------------------------------
param(
    [Parameter(Mandatory=$true)]
    [string]$BinDir
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Split-Path -Parent $ScriptDir

$leaf   = Split-Path -Leaf $BinDir
$IsDos  = ($leaf -match '^(dos-|DR|DT|DG)')

$CommName = if ($IsDos) { "COMMERCL.TXT" } else { "COMMERCIAL.md" }
$ExcpName = if ($IsDos) { "EXCEPT.TXT"   } else { "EXCEPTION.md" }

foreach ($f in (Resolve-Path "$RepoRoot\COPYING", "$RepoRoot\EXCEPTION.md",
                "$RepoRoot\COMMERCIAL.md", "$RepoRoot\LICENSING" -ErrorAction Stop)) {
    $name = Split-Path -Leaf $f
    if ($name -eq "COMMERCIAL.md") { $name = $CommName }
    if ($name -eq "EXCEPTION.md")  { $name = $ExcpName }
    Copy-Item $f (Join-Path $BinDir $name) -Force
}
