param(
    [string]$VcVars
)
$ErrorActionPreference = 'Stop'
if (-not $VcVars) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installPath) {
            $candidate = Join-Path $installPath 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path -LiteralPath $candidate) { $VcVars = $candidate }
        }
    }
}
if (-not $VcVars) {
    $defaultPaths = @(
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    )
    foreach ($p in $defaultPaths) {
        if (Test-Path -LiteralPath $p) { $VcVars = $p; break }
    }
}
if (-not $VcVars -or -not (Test-Path -LiteralPath $VcVars)) {
    throw "Visual Studio x64 Build Tools not found. Please install Visual Studio 2022 C++ tools or pass -VcVars explicitly."
}

Push-Location -LiteralPath $PSScriptRoot
try {
    $buildDir = Join-Path $PSScriptRoot 'build'
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

    $sources = @(
        'app.c',
        'driver_nvram.c',
        'boot_io.c',
        'msi_release.c',
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
