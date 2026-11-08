# SPDX-License-Identifier: GPL-2.0-only
# Common USB Deployment Module for Universal GPU Unlock

function Deploy-UniversalToUsb {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Drive,

        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [bool]$HideS3 = $false,

        [string]$AppVersion = "v1.7.9",

        [scriptblock]$OnLog = $null
    )

    function Emit-Log([string]$Message, [string]$Color = "") {
        if ($OnLog) {
            & $OnLog $Message $Color
        }
    }

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

    # 2. Copy AD102-ASSETS, GA102-ASSETS, TU102-ASSETS
    $assetsCopied = 0
    $ad102Src = Join-Path $RepoRoot "AD102-ASSETS"
    if (Test-Path -LiteralPath $ad102Src) {
        $assets = Get-ChildItem -LiteralPath $ad102Src -File
        foreach ($a in $assets) {
            Copy-Item -LiteralPath $a.FullName -Destination (Join-Path $targetAssetsDir $a.Name) -Force
            $assetsCopied++
        }
        Emit-Log "  [OK] Ada (AD102/AD103) assets copied to $driveRoot\AD102-ASSETS\" "Green"
    }

    $gaAssetsPath = Join-Path $RepoRoot 'GA102-ASSETS'
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

    $tu102Src = Join-Path $RepoRoot "TU102-ASSETS"
    if (Test-Path -LiteralPath $tu102Src) {
        $targetTuDir = Join-Path $driveRoot 'TU102-ASSETS'
        New-Item -ItemType Directory -Path $targetTuDir -Force | Out-Null
        $tuAssets = Get-ChildItem -LiteralPath $tu102Src -File
        foreach ($tu in $tuAssets) {
            Copy-Item -LiteralPath $tu.FullName -Destination (Join-Path $targetTuDir $tu.Name) -Force
            $assetsCopied++
        }
        $gspDst = Join-Path $targetTuDir "gsp_ga10x.bin"
        if (-not (Test-Path -LiteralPath $gspDst)) {
            $gspSrc = Join-Path $ad102Src "gsp_ga10x.bin"
            if (Test-Path -LiteralPath $gspSrc) {
                Copy-Item -LiteralPath $gspSrc -Destination $gspDst -Force
            }
        }
        Emit-Log "  [OK] Turing (TU102/TU104/TU106) assets copied to $driveRoot\TU102-ASSETS\" "Green"
    }

    $certSrc = Join-Path $RepoRoot "AdaUnlock_SecureBoot.cer"
    if (Test-Path -LiteralPath $certSrc) {
        Copy-Item -LiteralPath $certSrc -Destination (Join-Path $driveRoot "AdaUnlock_SecureBoot.cer") -Force
        Emit-Log "  [OK] Secure Boot certificate copied to $driveRoot\AdaUnlock_SecureBoot.cer" "Green"
    }

    # 3. Copy Setup and Benchmark Scripts
    $setupBat = Join-Path $RepoRoot "Setup.bat"
    $checkBat = Join-Path $RepoRoot "Check-GPU-Unlock.bat"
    $toolsDir = Join-Path $RepoRoot "tools"
    if (Test-Path -LiteralPath $setupBat) {
        Copy-Item -LiteralPath $setupBat -Destination (Join-Path $driveRoot "Setup.bat") -Force
    }
    if (Test-Path -LiteralPath $checkBat) {
        Copy-Item -LiteralPath $checkBat -Destination (Join-Path $driveRoot "Check-GPU-Unlock.bat") -Force
        $toolsDst = Join-Path $driveRoot "tools"
        New-Item -ItemType Directory -Path $toolsDst -Force | Out-Null
        $quickCheck = Join-Path $toolsDir "quick_check.py"
        if (Test-Path -LiteralPath $quickCheck) {
            Copy-Item -LiteralPath $quickCheck -Destination (Join-Path $toolsDst "quick_check.py") -Force
        }
        $uccCmd = Join-Path $RepoRoot "Universal-Control-Center.cmd"
        $uccPs1 = Join-Path $toolsDir "Universal-Control-Center.ps1"
        if (Test-Path -LiteralPath $uccCmd) { Copy-Item -LiteralPath $uccCmd -Destination (Join-Path $driveRoot "Universal-Control-Center.cmd") -Force }
        if (Test-Path -LiteralPath $uccPs1) { Copy-Item -LiteralPath $uccPs1 -Destination (Join-Path $toolsDst "Universal-Control-Center.ps1") -Force }
        Emit-Log "  [OK] Verification tools and Control Center copied to $driveRoot" "Green"
    }

    # 4. Write USB README
    $readmeContent = @"
# UNIVERSAL GPU UNLOCK $AppVersion - Bootable USB Drive
Hardware Rate-Limiter Override for Ada Lovelace, Ampere & Turing GPUs
Architect: BrightRaider | Desktop GUI: Ahsan Umair

## Boot Menu Modes:
- [U] Full GPU Unlock (Proven rock-solid, volatile RAM-only)
- [P] Experimental S3 Persistence Unlock (Boot-Script registration)
- [I] Install DXE Driver to UEFI NVRAM (Auto-boot on startup)
- [R] Remove DXE Driver from UEFI NVRAM
- [S] Stock Windows (Skip unlock, normal boot)

## Quick Start:
1. Reboot PC and press your Boot Menu key (F8 for ASUS, F11 for MSI/ASRock, F12 for Gigabyte).
2. Select your USB drive (e.g. 'UEFI: USB Drive').
3. Choose your desired mode ([U] for standard unlock, [P] for S3 persistence).
4. Windows boots with 100% full unrestricted tensor compute capacity!
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
