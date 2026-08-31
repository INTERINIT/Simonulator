[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string]$MameRoot,

    [string]$Msys2Root = 'C:\msys64',

    [ValidateRange(1, 64)]
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'

if ($Jobs -eq 0) {
    $Jobs = [Math]::Min(64, [Environment]::ProcessorCount + 1)
}

$MameRoot = (Resolve-Path -LiteralPath $MameRoot).Path
$Msys2Root = (Resolve-Path -LiteralPath $Msys2Root).Path
$bash = Join-Path $Msys2Root 'usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash -PathType Leaf)) {
    throw "MSYS2 Bash was not found: $bash"
}

& (Join-Path $PSScriptRoot 'Install-MameOverlay.ps1') -MameRoot $MameRoot

if ($MameRoot -notmatch '^([A-Za-z]):\\(.*)$') {
    throw "MameRoot must be on a local drive: $MameRoot"
}
$mamePosixPath = '/' + $Matches[1].ToLowerInvariant() + '/' + ($Matches[2] -replace '\\', '/')
$mamePosixPath = $mamePosixPath -replace "'", "'\\''"

$env:CHERE_INVOKING = 'yes'
$env:MSYSTEM = 'UCRT64'
$makeCommand = "cd -- '$mamePosixPath' && make SUBTARGET=ibmsimon SOURCES=src/mame/ibm/simon.cpp REGENIE=1 -j$Jobs"
& $bash -lc $makeCommand
if ($LASTEXITCODE -ne 0) {
    throw "MAME build failed with exit code $LASTEXITCODE"
}

$executable = Join-Path $MameRoot 'ibmsimon.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Build reported success but did not produce $executable"
}
Write-Host "Build complete: $executable"
