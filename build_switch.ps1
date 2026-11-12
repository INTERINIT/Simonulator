param([string]$Msys2Root)

$ErrorActionPreference = 'Stop'
if (!$Msys2Root) {
    $Msys2Root = Join-Path (Split-Path $PSScriptRoot -Parent) 'msys64'
    if (!(Test-Path -LiteralPath $Msys2Root)) { $Msys2Root = 'C:\msys64' }
}
$bash = (Resolve-Path -LiteralPath (Join-Path $Msys2Root 'usr/bin/bash.exe')).Path
$env:CHERE_INVOKING = 'yes'
$env:MSYSTEM = 'UCRT64'
Push-Location -LiteralPath $PSScriptRoot
try {
    & $bash -lc 'g++ -std=c++20 -O2 -Wall -Wextra -municode -mwindows -static amps_switch.cpp -o Simon_AMPS_Switch.exe -lsqlite3 -lbcrypt -lws2_32 -lcomctl32 -lgdi32 -lwinhttp'
    if ($LASTEXITCODE) { throw "AMPS switch build failed with exit code $LASTEXITCODE" }

    & $bash -lc 'g++ -std=c++20 -O2 -Wall -Wextra -municode -static weber_serial.cpp -o SimonWeber_Modem_Server.exe -lwinhttp'
    if ($LASTEXITCODE) { throw "SimonWeber serial server build failed with exit code $LASTEXITCODE" }
    Get-Item -LiteralPath 'Simon_AMPS_Switch.exe', 'SimonWeber_Modem_Server.exe'
} finally { Pop-Location }
