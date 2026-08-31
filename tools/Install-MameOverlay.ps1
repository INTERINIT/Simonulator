[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string]$MameRoot
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$MameRoot = (Resolve-Path -LiteralPath $MameRoot).Path
$listPath = Join-Path $MameRoot 'src\mame\mame.lst'
if (-not (Test-Path -LiteralPath $listPath -PathType Leaf)) {
    throw "Not a MAME source tree (missing $listPath)"
}

$overlayFiles = @(
    @{ Source = Join-Path $repositoryRoot 'src\mame\ibm\simon.cpp'; Destination = Join-Path $MameRoot 'src\mame\ibm\simon.cpp' },
    @{ Source = Join-Path $repositoryRoot 'src\mame\layout\ibmsimon.lay'; Destination = Join-Path $MameRoot 'src\mame\layout\ibmsimon.lay' }
)

foreach ($file in $overlayFiles) {
    if (-not (Test-Path -LiteralPath $file.Source -PathType Leaf)) {
        throw "Overlay source is missing: $($file.Source)"
    }
    $parent = Split-Path -Parent $file.Destination
    if ($PSCmdlet.ShouldProcess($parent, "Create directory for $($file.Destination)")) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    if ($PSCmdlet.ShouldProcess($file.Destination, "Install $($file.Source)")) {
        Copy-Item -LiteralPath $file.Source -Destination $file.Destination -Force
    }
}

# The driver list is required for this system to be registered in a MAME
# subtarget.  Insert our entry before the nearby IBM Portable PC source block;
# the operation is deliberately idempotent.
$listText = [System.IO.File]::ReadAllText($listPath)
if ($listText -notmatch '(?m)^@source:ibm/simon\.cpp\r?$') {
	$newLine = if ($listText.Contains("`r`n")) { "`r`n" } else { "`n" }
	$entry = "@source:ibm/simon.cpp${newLine}ibmsimon${newLine}${newLine}"
    $anchor = '@source:ibm/ptpc110.cpp'
    $index = $listText.IndexOf($anchor, [System.StringComparison]::Ordinal)
    if ($index -lt 0) {
        throw "Could not find expected insertion point '$anchor' in $listPath"
    }
    $updatedText = $listText.Insert($index, $entry)
    if ($PSCmdlet.ShouldProcess($listPath, 'Register ibmsimon driver')) {
        [System.IO.File]::WriteAllText($listPath, $updatedText, [System.Text.UTF8Encoding]::new($false))
    }
}

Write-Host "Simonulator overlay installed in $MameRoot"
