# SPDX-License-Identifier: GPL-2.0-only
# Helper to locate 64-bit MSVC environment (vcvars64.bat)

function Find-VcVars {
    [CmdletBinding()]
    param(
        [string]$VcVars
    )

    if ($VcVars -and (Test-Path -LiteralPath $VcVars)) {
        return $VcVars
    }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installPath) {
            $candidate = Join-Path $installPath 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path -LiteralPath $candidate) { return $candidate }
        }
    }

    $defaultPaths = @(
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    )
    foreach ($p in $defaultPaths) {
        if (Test-Path -LiteralPath $p) { return $p }
    }

    throw "Visual Studio x64 Build Tools not found. Please install Visual Studio 2022 C++ tools or pass -VcVars explicitly."
}
