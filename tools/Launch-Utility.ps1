# Universal GPU Unlock - PowerShell Launcher
[CmdletBinding()]
param(
    [switch]$Cli,
    [string]$Action = 'Audit',
    [string]$DriveLetter,
    [string]$CapturePath,
    [string]$LeftReport,
    [string]$RightReport,
    [string]$ExportPath,
    [string]$Lang = 'en',
    [switch]$Json
)

$script = Join-Path $PSScriptRoot "Universal-Control-Center.ps1"
& $script @PSBoundParameters
