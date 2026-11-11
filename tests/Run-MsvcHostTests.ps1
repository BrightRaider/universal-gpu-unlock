# SPDX-License-Identifier: GPL-2.0-only
# Builds and runs the MSVC host tests for UEFI code paths that need EDK2 types
# (driver_nvram.c, boot_io.c). Firmware services are mocked; no hardware is used.
param(
    [string]$VcVars,
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $repo 'src\Find-VcVars.ps1')
$VcVars = Find-VcVars $VcVars
if (-not $OutDir) { $OutDir = Join-Path ([System.IO.Path]::GetTempPath()) 'gu-msvc-host-tests' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$src = Join-Path $repo 'src'
$includes = @('.', 'compat', 'vendor', 'vendor\edk2\Include', 'vendor\edk2\Include\X64') |
    ForEach-Object { '/I"' + (Join-Path $src $_) + '"' }
$flags = '/nologo /W3 /TC /D__KERNEL__ /DGU_UEFI_PORT /DGU_AD104_TRANSACTION /DGU_ENTRY_LATCHED_SS1_FULL /D_CRT_SECURE_NO_WARNINGS ' + ($includes -join ' ')

$tests = @('test_driver_nvram', 'test_boot_select', 'test_csm_handover')
$lines = @('@echo off', "call `"$VcVars`" >nul", 'if errorlevel 1 exit /b 1', "cd /d `"$OutDir`"")
foreach ($test in $tests) {
    $source = Join-Path $PSScriptRoot "$test.c"
    $lines += "cl $flags /Fe$test.exe `"$source`" /link /subsystem:console"
    $lines += 'if errorlevel 1 exit /b 1'
    $lines += "`"$(Join-Path $OutDir "$test.exe")`""
    $lines += 'if errorlevel 1 exit /b 1'
}
$batch = Join-Path $OutDir 'run-msvc-host-tests.cmd'
Set-Content -LiteralPath $batch -Value $lines -Encoding ascii
& $env:ComSpec /d /c $batch
if ($LASTEXITCODE -ne 0) { throw "MSVC host tests failed (exit=$LASTEXITCODE)" }
Write-Output 'MSVC_HOST_TESTS=PASS'
