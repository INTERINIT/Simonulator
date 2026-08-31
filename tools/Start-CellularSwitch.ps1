[CmdletBinding()]
param(
    [int]$Port = 5555,
    [int]$FirstNumber = 1001,
    [ValidateSet('HOME1','HOME2','HOME3','HOME4','ROAM','ALTROAM','OFFLINE','NOSERVICE')]
    [string]$DefaultRegistration = 'HOME1',
    [ValidateRange(0, 6)]
    [int]$DefaultSignal = 6,
    [string]$DefaultOperator = 'Virtual AMPS',
    [switch]$TraceRF,
    [switch]$Headless
)

& (Join-Path $PSScriptRoot 'cellular_broker.ps1') @PSBoundParameters
