[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [string]$MameRoot,
    [string]$OverlayRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$MameRoot = (Resolve-Path -LiteralPath $MameRoot).Path
$OverlayRoot = (Resolve-Path -LiteralPath $OverlayRoot).Path

# Each path is relative to the upstream MAME tree. Keeping an explicit list
# also lets a missing release source fail before any destination is changed.
$files = @(
    'scripts/src/main.lua'
    'scripts/src/osd/sdl3.lua'
    'src/devices/bus/pccard/sram.cpp'
    'src/devices/bus/pccard/sram.h'
    'src/devices/machine/genpc.cpp'
    'src/devices/machine/genpc.h'
    'src/devices/machine/pckeybrd.cpp'
    'src/emu/crsshair.cpp'
    'src/emu/crsshair.h'
    'src/frontend/mame/ui/ui.cpp'
    'src/frontend/mame/ui/ui.h'
    'src/mame/ibm/simon.cpp'
    'src/mame/ibm/simon_kbd.cpp'
    'src/mame/ibm/simon_kbd.h'
    'src/mame/layout/ibmsimon.lay'
    'src/osd/modules/lib/osdlib_win32.cpp'
    'src/osd/sdl3/android_main.cpp'
    'src/osd/windows/window.cpp'
    'src/osd/windows/window.h'
)

$listPath = Join-Path $MameRoot 'src/mame/mame.lst'
$list = [IO.File]::ReadAllText($listPath)
foreach ($relative in $files) {
    $source = Join-Path $OverlayRoot $relative
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Missing overlay source: $source"
    }
}
if ($OverlayRoot -ne $MameRoot) {
    foreach ($relative in $files) {
        $source = Join-Path $OverlayRoot $relative
        $target = Join-Path $MameRoot $relative
        if ($PSCmdlet.ShouldProcess($target, 'Install MAME source')) {
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
            Copy-Item -LiteralPath $source -Destination $target -Force
        }
    }
}
if ($list -notmatch '(?m)^@source:ibm/simon\.cpp\r?$') {
    $nl = if ($list.Contains("`r`n")) { "`r`n" } else { "`n" }
    $entry = "@source:ibm/simon.cpp${nl}ibmsimon${nl}${nl}"
    $offset = $list.IndexOf('@source:ibm/ptpc110.cpp', [StringComparison]::Ordinal)
    if ($offset -lt 0) { $list = $list.TrimEnd() + $nl + $nl + $entry }
    else { $list = $list.Insert($offset, $entry) }
    if ($PSCmdlet.ShouldProcess($listPath, 'Register ibmsimon')) {
        [IO.File]::WriteAllText($listPath, $list, [Text.UTF8Encoding]::new($false))
    }
}
