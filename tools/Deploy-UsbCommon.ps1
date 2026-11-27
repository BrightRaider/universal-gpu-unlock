# SPDX-License-Identifier: GPL-2.0-only
# Common USB Deployment Module for Universal GPU Unlock

# The single source of the version is src/version.h. Returns 'unknown' if it is missing.
function Get-GuVersion([string]$RepoRoot) {
    $versionH = Join-Path $RepoRoot 'src\version.h'
    if (Test-Path -LiteralPath $versionH) {
        if ((Get-Content -LiteralPath $versionH -Raw) -match '#define\s+GU_VERSION_TAG\s+"([^"]+)"') {
            return $matches[1]
        }
    }
    return 'unknown'
}

# The single source of supported GPUs is src/gpu_device_ids.h, the same list the
# bootloader is built from. Returns a hashtable keyed by the lower-case 4-digit
# device ID, each value @{ Arch = 'Ada'|'Ampere'|'Turing'; Family = 'AD102'...; Name = ... }.
# Name is only set when the header comment names exactly one card for that ID.
function Get-GuSupportedGpuIds([string]$RepoRoot) {
    $table = @{}
    $header = Join-Path $RepoRoot 'src\gpu_device_ids.h'
    if (-not (Test-Path -LiteralPath $header)) { return $table }
    $arch = $null
    foreach ($line in Get-Content -LiteralPath $header) {
        if ($line -match '#define\s+GU_(ADA|AMPERE|TURING)_DEVICE_IDS') {
            $arch = @{ ADA = 'Ada'; AMPERE = 'Ampere'; TURING = 'Turing' }[$matches[1]]
        }
        if (-not $arch) { continue }
        $ids = @([regex]::Matches($line, 'X\(0x([0-9A-Fa-f]{4})U\)') | ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() })
        $comment = if ($line -match '/\*\s*(.*?)\s*\*/') { $matches[1] } else { '' }
        $family = if ($comment -match '\(((?:AD|GA|TU)\d{3})\)') { $matches[1] } else { @{ Ada = 'AD10x'; Ampere = 'GA10x'; Turing = 'TU10x' }[$arch] }
        foreach ($id in $ids) {
            $name = if ($ids.Count -eq 1 -and $comment -and $comment -notmatch ',') { ($comment -replace '\s*\([^)]*\)\s*$', '') } else { $null }
            $table[$id] = @{ Arch = $arch; Family = $family; Name = $name }
        }
        if (-not $line.TrimEnd().EndsWith('\')) { $arch = $null }
    }
    return $table
}

# Everything the Control Center, Dashboard, Setup and Benchmark need on the USB drive under tools\.
function Get-GuUsbToolFiles {
    return @(
        'quick_check.py',
        'gpu_dashboard.py',
        'dashboard_i18n.py',
        'gu_system_info.py',
        'Deploy-UsbCommon.ps1',
        'Setup-AdaUnlock.ps1',
        'Update-USB.bat',
        'Create-Unlock-Partition.bat',
        'Create-Unlock-Partition.ps1',
        'Setup-FastResume.bat',
        'Export-Boot-Log.ps1',
        'Universal-Control-Center.ps1',
        'Universal-Control-Center.xaml',
        'Universal-Control-Center-Detection.ps1',
        'Universal-Control-Center-Deployment.ps1',
        'Universal-Control-Center-Diagnostics.ps1',
        'Universal-Control-Center.cmd',
        'Toggle-Turing-GSP.bat',
        'Toggle-Sleep-Button.bat'
    )
}

function Deploy-UniversalToUsb {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Drive,

        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [bool]$HideS3 = $false,

        [string]$AppVersion = '',

        [scriptblock]$OnLog = $null
    )

    function Emit-Log([string]$Message, [string]$Color = "") {
        if ($OnLog) {
            & $OnLog $Message $Color
        }
    }

    if (-not $AppVersion) { $AppVersion = Get-GuVersion $RepoRoot }

    $cleanLetter = $Drive.TrimEnd(':\')
    $driveRoot = "$($cleanLetter):\"
    if (-not (Test-Path -LiteralPath $driveRoot)) {
        throw "Target drive '$driveRoot' does not exist."
    }

    $vol = Get-Volume -DriveLetter $cleanLetter -ErrorAction SilentlyContinue
    if ($vol -and $vol.FileSystem -ne 'FAT32') {
        throw "Drive $($cleanLetter): is formatted as $($vol.FileSystem). UEFI requires FAT32."
    }

    $targetBootDir = Join-Path $driveRoot "EFI\BOOT"
    $targetDriversDir = Join-Path $driveRoot "EFI\Drivers"
    $targetAssetsDir = Join-Path $driveRoot "AD102-ASSETS"

    New-Item -ItemType Directory -Path $targetBootDir -Force | Out-Null
    New-Item -ItemType Directory -Path $targetDriversDir -Force | Out-Null
    New-Item -ItemType Directory -Path $targetAssetsDir -Force | Out-Null

    # Handle S3 Menu Option
    $hideS3Cfg = Join-Path $targetBootDir "hide_s3.cfg"
    if ($HideS3) {
        Set-Content -LiteralPath $hideS3Cfg -Value "HIDE_S3=1" -Encoding ascii
        Emit-Log "  [OK] Bootloader configured to hide S3: [U/I/R/S] menu active." "Green"
    } else {
        if (Test-Path -LiteralPath $hideS3Cfg) { Remove-Item -LiteralPath $hideS3Cfg -Force }
        Emit-Log "  [OK] Bootloader configured with full options: [U/P/I/R/S] menu active." "Green"
    }

    # 1. Copy Universal Bootloader & DXE Driver
    $releaseEfi = Join-Path $RepoRoot "bin\BOOTX64.EFI"
    if (Test-Path -LiteralPath $releaseEfi) {
        Copy-Item -LiteralPath $releaseEfi -Destination (Join-Path $targetBootDir "BOOTX64.EFI") -Force
        Emit-Log "  [OK] UEFI Bootloader copied to $driveRoot\EFI\BOOT\BOOTX64.EFI" "Green"
    } else {
        throw "BOOTX64.EFI missing from bin/"
    }

    $dxeEfi = Join-Path $RepoRoot "bin\AdaUnlockDxe.efi"
    if (Test-Path -LiteralPath $dxeEfi) {
        Copy-Item -LiteralPath $dxeEfi -Destination (Join-Path $targetDriversDir "AdaUnlockDxe.efi") -Force
        Copy-Item -LiteralPath $dxeEfi -Destination (Join-Path $targetBootDir "AdaUnlockDxe.efi") -Force
        Emit-Log "  [OK] DXE Driver copied to $driveRoot\EFI\Drivers\AdaUnlockDxe.efi" "Green"
    }

    function Resolve-GuAssetSourceDir([string]$FolderName) {
        $nested = Join-Path (Join-Path $RepoRoot 'assets') $FolderName
        if (Test-Path -LiteralPath $nested) { return $nested }
        return (Join-Path $RepoRoot $FolderName)
    }

    # 2. Copy AD102-ASSETS, GA102-ASSETS, TU102-ASSETS (repo assets/ -> USB root for EFI/NVRAM compatibility)
    $assetsCopied = 0
    $ad102Src = Resolve-GuAssetSourceDir 'AD102-ASSETS'
    if (Test-Path -LiteralPath $ad102Src) {
        $assets = Get-ChildItem -LiteralPath $ad102Src -File
        foreach ($a in $assets) {
            Copy-Item -LiteralPath $a.FullName -Destination (Join-Path $targetAssetsDir $a.Name) -Force
            $assetsCopied++
        }
        Emit-Log "  [OK] Ada (AD102/AD103) assets copied to $driveRoot\AD102-ASSETS\" "Green"
    }

    $gaAssetsPath = Resolve-GuAssetSourceDir 'GA102-ASSETS'
    if (Test-Path -LiteralPath $gaAssetsPath) {
        $targetGaDir = Join-Path $driveRoot 'GA102-ASSETS'
        New-Item -ItemType Directory -Path $targetGaDir -Force | Out-Null
        $gaAssets = Get-ChildItem -LiteralPath $gaAssetsPath -File
        foreach ($ga in $gaAssets) {
            Copy-Item -LiteralPath $ga.FullName -Destination (Join-Path $targetGaDir $ga.Name) -Force
            $assetsCopied++
        }
        $gspDst = Join-Path $targetGaDir "gsp_ga10x.bin"
        if (-not (Test-Path -LiteralPath $gspDst)) {
            $gspSrc = Join-Path $ad102Src "gsp_ga10x.bin"
            if (Test-Path -LiteralPath $gspSrc) {
                Copy-Item -LiteralPath $gspSrc -Destination $gspDst -Force
            }
        }
        Emit-Log "  [OK] Ampere (GA102) assets copied to $driveRoot\GA102-ASSETS\" "Green"
    }

    $tu102Src = Resolve-GuAssetSourceDir 'TU102-ASSETS'
    if (Test-Path -LiteralPath $tu102Src) {
        $targetTuDir = Join-Path $driveRoot 'TU102-ASSETS'
        New-Item -ItemType Directory -Path $targetTuDir -Force | Out-Null
        $tuAssets = Get-ChildItem -LiteralPath $tu102Src -File
        foreach ($tu in $tuAssets) {
            Copy-Item -LiteralPath $tu.FullName -Destination (Join-Path $targetTuDir $tu.Name) -Force
            $assetsCopied++
        }
        Emit-Log "  [OK] Turing (TU102/TU104/TU106) assets copied to $driveRoot\TU102-ASSETS\" "Green"
    }

    $certSrc = Join-Path $RepoRoot "AdaUnlock_SecureBoot.cer"
    if (Test-Path -LiteralPath $certSrc) {
        Copy-Item -LiteralPath $certSrc -Destination (Join-Path $driveRoot "AdaUnlock_SecureBoot.cer") -Force
        Emit-Log "  [OK] Secure Boot certificate copied to $driveRoot\AdaUnlock_SecureBoot.cer" "Green"
    }

    # 3. Copy clean Root Launchers (Setup, Dashboard, Benchmark) + full tools/ suite
    $setupBat = Join-Path $RepoRoot "Setup.bat"
    $dashBat = Join-Path $RepoRoot "Launch-Dashboard.bat"
    $checkBat = Join-Path $RepoRoot "Check-GPU-Unlock.bat"
    $toolsDir = Join-Path $RepoRoot "tools"
    if (Test-Path -LiteralPath $setupBat) {
        Copy-Item -LiteralPath $setupBat -Destination (Join-Path $driveRoot "Setup.bat") -Force
    }
    if (Test-Path -LiteralPath $dashBat) {
        Copy-Item -LiteralPath $dashBat -Destination (Join-Path $driveRoot "Launch-Dashboard.bat") -Force
    }
    if (Test-Path -LiteralPath $checkBat) {
        Copy-Item -LiteralPath $checkBat -Destination (Join-Path $driveRoot "Check-GPU-Unlock.bat") -Force
        $toolsDst = Join-Path $driveRoot "tools"
        New-Item -ItemType Directory -Path $toolsDst -Force | Out-Null
        foreach ($file in (Get-GuUsbToolFiles)) {
            $source = Join-Path $toolsDir $file
            if (-not (Test-Path -LiteralPath $source)) {
                $source = Join-Path $RepoRoot $file
            }
            if (Test-Path -LiteralPath $source) {
                Copy-Item -LiteralPath $source -Destination (Join-Path $toolsDst $file) -Force
            } else {
                Emit-Log "  [WARN] Missing tool file: $file" "Yellow"
            }
        }
        # Version and supported-GPU list, read by Get-GuVersion and Get-GuSupportedGpuIds on the stick.
        $srcDst = Join-Path $driveRoot "src"
        New-Item -ItemType Directory -Path $srcDst -Force | Out-Null
        foreach ($file in 'version.h', 'gpu_device_ids.h') {
            $source = Join-Path (Join-Path $RepoRoot "src") $file
            if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $srcDst $file) -Force }
        }
        Emit-Log "  [OK] Dashboard, Verification tools and Control Center copied to $driveRoot" "Green"
    }

    # 4. Write USB README
    $readmeContent = @"
# UNIVERSAL GPU UNLOCK $AppVersion - Bootable USB Drive
Hardware Rate-Limiter Override for Ada Lovelace, Ampere & Turing GPUs
Architect: BrightRaider | Desktop GUI: Ahsan Umair

## Boot Menu Modes:
- [U] Full GPU Unlock (Proven rock-solid, volatile RAM-only)
- [P] S3 Sleep Persistence Unlock (Tier 1 Boot-Script + Tier 2 ACPI FACS Trampoline)
- [I] Install DXE Driver to Internal ESP / GPU_UNLOCK & UEFI NVRAM (Stickless auto-boot)
- [R] Remove DXE Driver from Internal ESP & UEFI NVRAM
- [S] Stock Windows (Skip unlock, normal boot)

## Quick Start:
1. Reboot PC and press your Boot Menu key (F8 for ASUS, F11 for MSI/ASRock, F12 for Gigabyte).
2. Select your USB drive (e.g. 'UEFI: USB Drive').
3. Choose your desired mode ([U] for standard unlock, [P] for S3 persistence, [I] for stickless boot).
4. In Windows, run Launch-Dashboard.bat (GUI Cockpit) or Check-GPU-Unlock.bat (CLI Benchmark).
"@
    Set-Content -LiteralPath (Join-Path $driveRoot "README-USB.txt") -Value $readmeContent -Encoding utf8

    return [pscustomobject]@{
        Drive        = $driveRoot
        BootEfi      = (Join-Path $targetBootDir "BOOTX64.EFI")
        AssetsCopied = $assetsCopied
        Status       = "Universal 1-Click Deployment Complete! Bootloader and assets staged."
        IsProbe      = $false
        ProfileMode  = "Universal"
        RequiresBoot = $false
    }
}
