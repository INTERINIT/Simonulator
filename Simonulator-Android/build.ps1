param(
    [string]$Sdk = $env:ANDROID_HOME,
    [string]$Ndk = $env:ANDROID_NDK_HOME,
    [string]$Msys2Root,
    [int]$Jobs = 5
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$mame = Join-Path $root 'mame'
if (!$Msys2Root) {
    $Msys2Root = Join-Path $root 'msys64'
    if (!(Test-Path -LiteralPath $Msys2Root)) { $Msys2Root = 'C:\msys64' }
}
$bash = (Resolve-Path -LiteralPath (Join-Path $Msys2Root 'usr/bin/bash.exe')).Path
# Source releases keep the overlay beside this project and the upstream
# checkout in ../mame. A development checkout already contains the driver.
if (Test-Path -LiteralPath (Join-Path $root 'src/mame/ibm/simon.cpp')) {
    & (Join-Path $root 'tools/Install-MameOverlay.ps1') -MameRoot $mame
}
if (!$Sdk) { throw 'Set ANDROID_HOME to an SDK containing Android 35.' }
if (!$Ndk) { $Ndk = Join-Path $Sdk 'ndk/27.2.12479018' }
$env:ANDROID_HOME = (Resolve-Path -LiteralPath $Sdk).Path
$env:ANDROID_NDK_HOME = (Resolve-Path -LiteralPath $Ndk).Path.Replace('\','/')
$env:SDL_INSTALL_ROOT = (Join-Path $PSScriptRoot 'build/sdl').Replace('\','/')
$env:CHERE_INVOKING = 'yes'
$env:MSYSTEM = 'UCRT64'
& python (Join-Path $PSScriptRoot 'prepare_sdl.py')
if ($LASTEXITCODE) { throw 'SDL preparation failed.' }
Push-Location -LiteralPath $mame
try {
    & $bash -lc "make android-arm64 SUBTARGET=ibmsimon SOURCES=src/mame/ibm/simon.cpp BUSES=PCCARD MACHINES=KEYBOARD,CASSETTE SOUNDS=DAC OSD=sdl3 TARGETOS=android PTR64=1 NO_USE_PORTAUDIO=1 USE_QTDEBUG=0 NOASM=1 BUILDDIR=build/android-simon -j$Jobs"
    if ($LASTEXITCODE) { throw 'Android native build failed.' }
} finally { Pop-Location }
& (Join-Path $root 'mame/android-project/gradlew.bat') -p $PSScriptRoot assembleDebug
if ($LASTEXITCODE) { throw 'Android package build failed.' }
