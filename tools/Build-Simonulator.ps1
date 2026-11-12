[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$MameRoot,
    [string]$Msys2Root = 'C:\msys64',
    [ValidateRange(1, 64)] [int]$Jobs = [Math]::Min(64, [Environment]::ProcessorCount),
    [string]$OverlayRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$bash = (Resolve-Path -LiteralPath (Join-Path $Msys2Root 'usr/bin/bash.exe')).Path
& (Join-Path $PSScriptRoot 'Install-MameOverlay.ps1') -MameRoot $MameRoot -OverlayRoot $OverlayRoot
$env:CHERE_INVOKING = 'yes'
$env:MSYSTEM = 'UCRT64'
Push-Location -LiteralPath $MameRoot
try {
    & $bash -lc "make SUBTARGET=ibmsimon SOURCES=src/mame/ibm/simon.cpp BUSES=PCCARD MACHINES=KEYBOARD,CASSETTE SOUNDS=DAC REGENIE=1 -j$Jobs"
    if ($LASTEXITCODE) { throw "MAME build failed with exit code $LASTEXITCODE" }
    Get-Item -LiteralPath 'ibmsimon.exe'
} finally { Pop-Location }
