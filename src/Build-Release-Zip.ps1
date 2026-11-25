# SPDX-License-Identifier: GPL-2.0-only
# Packages a clean Universal GPU Unlock release archive directly from git HEAD,
# honoring .gitattributes (export-ignore) and guaranteeing gitignored local/dev
# files (REDDIT_POST.md, scratch/, *.obj, build dirs) are excluded.
param(
    [string]$OutPath,
    [string]$Prefix = '',
    [switch]$AllowDirty,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $repoRoot 'tools\Deploy-UsbCommon.ps1')

$ver = Get-GuVersion -RepoRoot $repoRoot
if (-not $ver -or $ver -eq 'unknown') {
    throw "Could not resolve version from src\version.h"
}

if (-not $OutPath) {
    $OutPath = Join-Path $repoRoot ("Universal_GPU_Unlock_${ver}.zip")
}

Push-Location -LiteralPath $repoRoot
try {
    if (-not $AllowDirty) {
        $dirty = git status -s --untracked-files=no
        if ($LASTEXITCODE -ne 0) { throw "git status failed" }
        if ($dirty) {
            throw "Working tree has uncommitted tracked changes. Commit first or pass -AllowDirty.`n$dirty"
        }
    }

    # Verify bin/ binaries and co-located docs match root before archiving
    foreach ($efi in @('bin\BOOTX64.EFI', 'bin\AdaUnlockDxe.efi')) {
        if (-not (Test-Path -LiteralPath $efi)) { throw "Missing binary: $efi" }
        $hit = Select-String -Path $efi -Pattern ([regex]::Escape($ver)) -Quiet
        if (-not $hit) { throw "$efi does not contain version string '$ver'" }
    }
    foreach ($pair in @(
        @('README.md', 'bin\README.md'),
        @('DOCUMENTATION.md', 'bin\DOCUMENTATION.md'),
        @('CHANGELOG.md', 'bin\CHANGELOG.md'),
        @('src\version.h', 'bin\version.h')
    )) {
        $h1 = (Get-FileHash -LiteralPath $pair[0] -Algorithm SHA256).Hash
        $h2 = (Get-FileHash -LiteralPath $pair[1] -Algorithm SHA256).Hash
        if ($h1 -ne $h2) { throw "Co-located copy out of sync: $($pair[0]) != $($pair[1])" }
    }

    if ($DryRun) {
        Write-Output "DRY_RUN=OK VERSION=$ver TARGET=$OutPath"
        return
    }

    $outDir = Split-Path -Parent $OutPath
    if ($outDir) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
    if (Test-Path -LiteralPath $OutPath) { Remove-Item -LiteralPath $OutPath -Force }

    $archiveArgs = @('archive', '--format=zip', '--worktree-attributes')
    if ($Prefix) {
        $normPrefix = $Prefix.TrimEnd('/') + '/'
        $archiveArgs += "--prefix=$normPrefix"
    }
    $archiveArgs += @('-o', $OutPath, 'HEAD')

    & git @archiveArgs
    if ($LASTEXITCODE -ne 0) { throw "git archive failed with exit code $LASTEXITCODE" }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($OutPath)
    try {
        $entries = @($zip.Entries | ForEach-Object { $_.FullName })
    } finally {
        $zip.Dispose()
    }

    $forbidden = @($entries | Where-Object {
        $_ -match '(^|/)(REDDIT_POST\.md|\.gitignore|\.gitattributes|scratch/|\.github/|.*\.obj$|.*\.o$|.*\.tmp$)'
    })
    if ($forbidden.Count -gt 0) {
        Remove-Item -LiteralPath $OutPath -Force -ErrorAction SilentlyContinue
        throw ("Forbidden dev/meta entries found in release ZIP: " + ($forbidden -join ', '))
    }

    $hash = (Get-FileHash -LiteralPath $OutPath -Algorithm SHA256).Hash
    $sizeMb = [math]::Round(((Get-Item -LiteralPath $OutPath).Length / 1MB), 2)
    Write-Output "RELEASE_ZIP=$OutPath"
    Write-Output "VERSION=$ver ENTRIES=$($entries.Count) SIZE_MB=$sizeMb SHA256=$hash"
} finally {
    Pop-Location
}
