[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string]$MameRoot,

    [Parameter(Mandatory)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string]$FirmwareRoot,

    [string]$CellularEndpoint = ''
)

$ErrorActionPreference = 'Stop'
$MameRoot = (Resolve-Path -LiteralPath $MameRoot).Path
$FirmwareRoot = (Resolve-Path -LiteralPath $FirmwareRoot).Path
$exe = Join-Path $MameRoot 'ibmsimon.exe'
$romSet = Join-Path $FirmwareRoot 'ibmsimon'

if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Missing emulator: $exe" }
if (-not (Test-Path -LiteralPath (Join-Path $romSet 'simonbios.bin') -PathType Leaf)) { throw "Missing simonbios.bin in $romSet" }
if (-not (Test-Path -LiteralPath (Join-Path $romSet 'simonflash.bin') -PathType Leaf)) { throw "Missing simonflash.bin in $romSet" }

$arguments = @('ibmsimon', '-rompath', $FirmwareRoot, '-window', '-nomaximize', '-skip_gameinfo', '-lightgun', '-lightgun_device', 'mouse')
if ($CellularEndpoint) { $arguments += @('-bitb', $CellularEndpoint) }
& $exe @arguments
