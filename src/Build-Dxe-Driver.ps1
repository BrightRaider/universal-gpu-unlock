param(
    [string]$VcVars
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Find-VcVars.ps1')
$VcVars = Find-VcVars $VcVars

Push-Location -LiteralPath $PSScriptRoot
try {
    $buildDir = Join-Path $PSScriptRoot 'build_dxe'
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

    $sources = @(
        'dxe_main.c',
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
        $obj = "build_dxe\$base.obj"
        $objFiles += $obj
        $flags = $clFlags
        if ($src -eq 'runtime.c') {
            $flags = $clFlags.Replace('/Oi', '/Oi-')
        }
        $compileCmds += "cl $flags /Fobuild_dxe\$base.obj $src"
        $compileCmds += 'if errorlevel 1 exit /b 1'
    }

    $linkObjs = $objFiles -join ' '
    $compileCmds += "link /nologo /subsystem:efi_boot_service_driver /entry:AdaUnlockDriverEntryPoint /machine:x64 /nodefaultlib /incremental:no /fixed:no /dynamicbase:no /nxcompat:no /out:build_dxe\AdaUnlockDxe.efi $linkObjs"
    $compileCmds += 'if errorlevel 1 exit /b 1'

    $batchFile = Join-Path $buildDir 'build-dxe.cmd'
    Set-Content -LiteralPath $batchFile -Value $compileCmds -Encoding ascii

    & $env:ComSpec /d /c $batchFile
    if ($LASTEXITCODE -ne 0) { throw 'DXE Compilation failed' }

    Write-Output "Successfully compiled: build_dxe\AdaUnlockDxe.efi"
} finally {
    Pop-Location
}
