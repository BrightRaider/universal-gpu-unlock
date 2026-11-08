param(
    [string]$VcVars
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Find-VcVars.ps1')
$VcVars = Find-VcVars $VcVars

Push-Location -LiteralPath $PSScriptRoot
try {
    $buildDir = Join-Path $PSScriptRoot 'build'
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

    $sources = @(
        'app.c',
        'driver_nvram.c',
        'boot_io.c',
        'unlock_release.c',
        'port.c',
        'compat.c',
        'checks.c',
        'gpu_inventory.c',
        'gpu_discovery.c',
        'profile.c',
        'sha256.c',
        'runtime.c',
        'format.c',
        'vendor\unlock_610.c',
        'vendor\prep_610.c',
        'vendor\layout_610.c',
        'vendor\vbios_610.c',
        'vendor\gfw_eval.c'
    )

    $objFiles = @()
    $compileCmds = @(
        '@echo off',
        "call `"$VcVars`" >nul",
        'if errorlevel 1 exit /b 1',
        "cd /d `"$PSScriptRoot`""
    )

    $clFlags = '/nologo /O2 /Oi /GS- /Gs65536 /Zl /c /TC /D__KERNEL__ /DGU_UEFI_PORT /DGU_AD104_TRANSACTION /DGU_ENTRY_LATCHED_SS1_FULL /D_CRT_SECURE_NO_WARNINGS /I. /Icompat /Ivendor /Ivendor\edk2\Include /Ivendor\edk2\Include\X64'

    foreach ($src in $sources) {
        $base = [System.IO.Path]::GetFileNameWithoutExtension($src)
        if ($src -like 'vendor\*') { $base = 'vendor_' + $base }
        $obj = "build\$base.obj"
        $objFiles += $obj
        $flags = $clFlags
        if ($src -eq 'runtime.c') {
            $flags = $clFlags.Replace('/Oi', '/Oi-')
        }
        $compileCmds += "cl $flags /Fo$obj $src"
        $compileCmds += 'if errorlevel 1 exit /b 1'
    }

    $linkObjs = $objFiles -join ' '
    $compileCmds += "link /nologo /subsystem:efi_application /entry:UefiMain /machine:x64 /nodefaultlib /incremental:no /fixed:no /dynamicbase:no /nxcompat:no /out:build\AD102-EXPERIMENTAL.EFI $linkObjs"
    $compileCmds += 'if errorlevel 1 exit /b 1'

    $batchFile = Join-Path $buildDir 'build-msvc.cmd'
    Set-Content -LiteralPath $batchFile -Value $compileCmds -Encoding ascii

    & $env:ComSpec /d /c $batchFile
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }

    Write-Output "Successfully compiled: build\AD102-EXPERIMENTAL.EFI"
} finally {
    Pop-Location
}
