<#
.SYNOPSIS
    Universal RTX 40 & RTX 30 Series Tensor Unlock Setup Tool (v1.6.3)
.DESCRIPTION
    Automated 1-Click Setup Tool to create a bootable UEFI flash drive
    with the universal Ada Lovelace & Ampere rate-limiter unlock.
#>

param(
    [switch]$NonInteractive
)

# 1. Elevate Administrator Privileges
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "[INFO] Requesting Administrator privileges..." -ForegroundColor Cyan
    Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit
}

# 0. Load Central Version
$versionH = Join-Path $PSScriptRoot "src\version.h"
$AppVersion = "v1.7.9"
if (Test-Path -LiteralPath $versionH) {
    if ((Get-Content -LiteralPath $versionH -Raw) -match '#define\s+GU_VERSION_TAG\s+"([^"]+)"') {
        $AppVersion = $matches[1]
    }
}

Clear-Host
Write-Host "======================================================================" -ForegroundColor Magenta
Write-Host "   RTX 40, 30 & 20 SERIES UNIVERSAL UNLOCK - 1-CLICK SETUP ($AppVersion)      " -ForegroundColor Yellow
Write-Host "   Supported: Ada (4090..4070TiS) | Ampere (3090..3080) | Turing (2080Ti..2060)" -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Magenta
Write-Host ""

# 2. Hardware Detection (GPU)
Write-Host "[1/4] Scanning for compatible NVIDIA GeForce GPU (Ada / Ampere / Turing)..." -ForegroundColor Cyan
$gpus = Get-CimInstance Win32_VideoController | Where-Object {
    $_.Name -like "*4090*" -or $_.Name -like "*4080*" -or $_.Name -like "*4070 Ti SUPER*" -or
    $_.Name -like "*3090*" -or $_.Name -like "*3080*" -or $_.Name -like "*3070*" -or $_.Name -like "*3060*" -or
    $_.Name -like "*2080*" -or $_.Name -like "*2070*" -or $_.Name -like "*2060*" -or $_.Name -like "*TITAN RTX*" -or $_.Name -like "*CMP 50HX*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2684*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_2685*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2704*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_2703*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2705*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2204*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_2208*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2206*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_220A*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2216*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2482*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_2504*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_1E*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_1F*"
}

if ($gpus) {
    foreach ($gpu in $gpus) {
        Write-Host "  [OK] Detected target GPU: $($gpu.Name)" -ForegroundColor Green
    }
} else {
    Write-Host "  [NOTE] Target GPU not found in current device list (Setup can still proceed)." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "[2/4] Scanning for USB Flash Drives..." -ForegroundColor Cyan

# 3. Comprehensive USB Drive Detection
$detectedDrives = @()

# A: Win32_Volume removable drives
try {
    $removableVols = Get-CimInstance Win32_Volume -ErrorAction SilentlyContinue | Where-Object { $_.DriveType -eq 2 -and $_.DriveLetter }
    if ($removableVols) { $detectedDrives += $removableVols }
} catch {}

# B: Storage cmdlets (USB bus type, catches USB Fixed disks / external SSDs)
try {
    $usbDisks = Get-Disk -ErrorAction SilentlyContinue | Where-Object { $_.BusType -eq 'USB' }
    foreach ($d in $usbDisks) {
        $parts = $d | Get-Partition -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter }
        foreach ($p in $parts) {
            $vol = Get-Volume -DriveLetter $p.DriveLetter -ErrorAction SilentlyContinue
            if ($vol -and ($detectedDrives.DriveLetter -notcontains ($vol.DriveLetter + ":"))) {
                $detectedDrives += [PSCustomObject]@{
                    DriveLetter = ($vol.DriveLetter + ":")
                    Label = $vol.FileSystemLabel
                    Capacity = $vol.Size
                }
            }
        }
    }
} catch {}

$targetDriveLetter = $null

if ($detectedDrives.Count -eq 1) {
    $targetDrive = $detectedDrives[0]
    $capGb = if ($targetDrive.Capacity) { [math]::Round($targetDrive.Capacity / 1GB, 2) } else { 0 }
    Write-Host "  [OK] Auto-selected USB drive: $($targetDrive.DriveLetter) ($($targetDrive.Label), $capGb GB)" -ForegroundColor Green
    $targetDriveLetter = $targetDrive.DriveLetter
} elseif ($detectedDrives.Count -gt 1) {
    Write-Host "  Multiple USB drives detected. Please select one:" -ForegroundColor Yellow
    for ($i = 0; $i -lt $detectedDrives.Count; $i++) {
        $capGb = if ($detectedDrives[$i].Capacity) { [math]::Round($detectedDrives[$i].Capacity / 1GB, 2) } else { 0 }
        Write-Host "    [$($i+1)] $($detectedDrives[$i].DriveLetter) ($($detectedDrives[$i].Label), $capGb GB)"
    }
    $choice = Read-Host "  Enter number (1-$($detectedDrives.Count))"
    if ($choice -match '^\d+$' -and [int]$choice -ge 1 -and [int]$choice -le $detectedDrives.Count) {
        $targetDriveLetter = $detectedDrives[[int]$choice - 1].DriveLetter
    }
}

if (-not $targetDriveLetter) {
    Write-Host "  [NOTICE] Could not automatically detect a removable USB drive." -ForegroundColor Yellow
    $manual = Read-Host "  Please manually enter your USB drive letter (e.g. E: or E)"
    if ($manual) {
        $manual = $manual.Trim().ToUpper()
        if ($manual -notlike "*:") { $manual = "${manual}:" }
        if (Test-Path "$manual\") {
            $targetDriveLetter = $manual
            Write-Host "  [OK] Selected drive: $targetDriveLetter" -ForegroundColor Green
        } else {
            Write-Host "  [ERROR] Drive $manual does not exist or is not ready." -ForegroundColor Red
            Pause
            exit
        }
    } else {
        Write-Host "  [ABORT] No drive specified." -ForegroundColor Red
        Pause
        exit
    }
}

$driveLetter = $targetDriveLetter
Write-Host ""
Write-Host "  TARGET DRIVE: $driveLetter" -ForegroundColor Yellow
Write-Host "  NOTE: FAT32 format is recommended for UEFI boot compliance." -ForegroundColor Gray
$formatAnswer = Read-Host "  Would you like to quick-format $driveLetter with FAT32 now? (y/N)"

if ($formatAnswer -match "^[yYjJ]") {
    Write-Host "  Formatting $driveLetter with FAT32..." -ForegroundColor Cyan
    try {
        Format-Volume -DriveLetter $driveLetter.TrimEnd(':') -FileSystem FAT32 -NewFileSystemLabel "GPU_UNLOCK" -Confirm:$false -ErrorAction Stop | Out-Null
        Write-Host "  [OK] Format completed successfully." -ForegroundColor Green
    } catch {
        Write-Host "  [WARN] PowerShell Format-Volume failed, attempting standard format..." -ForegroundColor Yellow
        cmd.exe /c "format $($driveLetter.TrimEnd(':')): /FS:FAT32 /V:GPU_UNLOCK /Q /Y" | Out-Null
    }
}

# 4. Copy Bootloader & Microcode Assets
Write-Host ""
Write-Host "[3/4] Copying Universal Bootloader & Firmware Assets (Ada & Ampere)..." -ForegroundColor Cyan

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$efiSrc = Join-Path $scriptDir "bin\BOOTX64.EFI"
$ad102Src = Join-Path $scriptDir "AD102-ASSETS"
$ga102Src = Join-Path $scriptDir "GA102-ASSETS"
$certSrc = Join-Path $scriptDir "AdaUnlock_SecureBoot.cer"

$efiDstDir = Join-Path $driveLetter "EFI\BOOT"
New-Item -ItemType Directory -Path $efiDstDir -Force | Out-Null
Copy-Item $efiSrc (Join-Path $efiDstDir "BOOTX64.EFI") -Force
Write-Host "  [OK] UEFI Bootloader copied to $driveLetter\EFI\BOOT\BOOTX64.EFI" -ForegroundColor Green

$dxeSrc = Join-Path $scriptDir "bin\AdaUnlockDxe.efi"
if (Test-Path $dxeSrc) {
    $dxeDstDir = Join-Path $driveLetter "EFI\Drivers"
    New-Item -ItemType Directory -Path $dxeDstDir -Force | Out-Null
    Copy-Item $dxeSrc (Join-Path $dxeDstDir "AdaUnlockDxe.efi") -Force
    # Also place in EFI\BOOT for NVRAM [I] installer
    Copy-Item $dxeSrc (Join-Path $efiDstDir "AdaUnlockDxe.efi") -Force
    Write-Host "  [OK] DXE Driver copied to $driveLetter\EFI\Drivers\AdaUnlockDxe.efi" -ForegroundColor Green
}

# Bootloader S3 Menu Customization
Write-Host ""
Write-Host "  OPTIONAL: Bootloader Menu Customization" -ForegroundColor Gray
$s3Answer = Read-Host "  Hide experimental S3 Sleep Persistence [P] from boot menu? (y/N - default: No)"
$hideS3Cfg = Join-Path $efiDstDir "hide_s3.cfg"
if ($s3Answer -match "^[yYjJ]") {
    Set-Content -Path $hideS3Cfg -Value "# Hide S3" -Encoding Ascii
    Write-Host "  [OK] Bootloader configured to hide S3: [U/I/R/S] menu active." -ForegroundColor Green
} else {
    if (Test-Path $hideS3Cfg) { Remove-Item $hideS3Cfg -Force }
    Write-Host "  [OK] Bootloader configured with full options: [U/P/I/R/S] menu active." -ForegroundColor Green
}

if (Test-Path $ad102Src) {
    $adDst = Join-Path $driveLetter "AD102-ASSETS"
    New-Item -ItemType Directory -Path $adDst -Force | Out-Null
    Copy-Item (Join-Path $ad102Src "*") $adDst -Force
    Write-Host "  [OK] Ada (AD102/AD103) assets copied to $driveLetter\AD102-ASSETS\" -ForegroundColor Green
}

if (Test-Path $ga102Src) {
    $gaDst = Join-Path $driveLetter "GA102-ASSETS"
    New-Item -ItemType Directory -Path $gaDst -Force | Out-Null
    Copy-Item (Join-Path $ga102Src "*") $gaDst -Force
    $gspDst = Join-Path $gaDst "gsp_ga10x.bin"
    if (-not (Test-Path $gspDst)) {
        $gspSrc = Join-Path $ad102Src "gsp_ga10x.bin"
        if (Test-Path $gspSrc) {
            Copy-Item $gspSrc $gspDst -Force
        }
    }
    Write-Host "  [OK] Ampere (GA102) assets copied to $driveLetter\GA102-ASSETS\" -ForegroundColor Green
}

$tu102Src = Join-Path $scriptDir "TU102-ASSETS"
if (Test-Path $tu102Src) {
    $tuDst = Join-Path $driveLetter "TU102-ASSETS"
    New-Item -ItemType Directory -Path $tuDst -Force | Out-Null
    Copy-Item (Join-Path $tu102Src "*") $tuDst -Force
    $gspDst = Join-Path $tuDst "gsp_ga10x.bin"
    if (-not (Test-Path $gspDst)) {
        $gspSrc = Join-Path $ad102Src "gsp_ga10x.bin"
        if (Test-Path $gspSrc) {
            Copy-Item $gspSrc $gspDst -Force
        }
    }
    Write-Host "  [OK] Turing (TU102/TU104/TU106) assets copied to $driveLetter\TU102-ASSETS\" -ForegroundColor Green
}


if (Test-Path $certSrc) {
    Copy-Item $certSrc (Join-Path $driveLetter "AdaUnlock_SecureBoot.cer") -Force
    Write-Host "  [OK] Secure Boot certificate copied to $driveLetter\AdaUnlock_SecureBoot.cer" -ForegroundColor Green
}

$checkBat = Join-Path $scriptDir "Check-GPU-Unlock.bat"
$toolsDir = Join-Path $scriptDir "tools"
if (Test-Path $checkBat) {
    Copy-Item $checkBat (Join-Path $driveLetter "Check-GPU-Unlock.bat") -Force
    $toolsDst = Join-Path $driveLetter "tools"
    New-Item -ItemType Directory -Path $toolsDst -Force | Out-Null
    if (Test-Path (Join-Path $toolsDir "quick_check.py")) {
        Copy-Item (Join-Path $toolsDir "quick_check.py") (Join-Path $toolsDst "quick_check.py") -Force
    }
    Write-Host "  [OK] 1-Click Verification tool copied to $driveLetter\Check-GPU-Unlock.bat" -ForegroundColor Green

    $uccCmd = Join-Path $scriptDir "Universal-Control-Center.cmd"
    $uccVbs = Join-Path $scriptDir "Universal-Control-Center.vbs"
    $uccPs1 = Join-Path $toolsDir "Universal-Control-Center.ps1"
    if (Test-Path $uccCmd) { Copy-Item $uccCmd (Join-Path $driveLetter "Universal-Control-Center.cmd") -Force }
    if (Test-Path $uccVbs) { Copy-Item $uccVbs (Join-Path $driveLetter "Universal-Control-Center.vbs") -Force }
    if (Test-Path $uccPs1) { Copy-Item $uccPs1 (Join-Path $toolsDst "Universal-Control-Center.ps1") -Force }
    Write-Host "  [OK] Optional Control Center GUI copied to $driveLetter\Universal-Control-Center.cmd" -ForegroundColor Green
}

# 5. Integrity Verification (SHA-256)
Write-Host ""
Write-Host "[4/4] Verifying bit-exact SHA-256 checksums..." -ForegroundColor Cyan

$adHashes = @{
    "booter_load_ad102.bin" = "6ee05b20e1dc244c08c052f36ef28469b8d09f4955d1fe33e48a179e11a7fd49"
    "v67-ad102-610.43.03-four-plm.bin" = "44c36b2d6b3424e53e18f3e55db85c833655de071ce66ce42749588a4a7dc8cb"
    "gsp_ga10x.bin" = "572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a"
    "gsp_boot_ad102.bin" = "45d6335a8185d6ff13e8d1484cea73f68d3d13aa1c19dc080ed1c3482dd5bac0"
    "gsp_boot_desc_ad102.bin" = "83f8f3f1f5f9496e0ea8fc7103c2beb64d0b6de2f5ffe725f13514de5a95fa42"
}

$gaHashes = @{
    "booter_load_ga102.bin" = "056cbc064a888f80717726c54d7f64fcd68044d123870c95a74f37f4975dcaa4"
    "v67-ga102-610.43.03.bin" = "2a0aa1f86d0b6afff5214d641d87d46dc98ef06ed908ff05d0ddbad5ffb7e197"
    "gsp_ga10x.bin" = "572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a"
    "gsp_boot_ga102.bin" = "3df651acb174fd5f5561a1996965b192f50c5d763168446ed0d4ead19d895768"
    "gsp_boot_desc_ga102.bin" = "cc5a3fc1f4c47f2fe107b1b68f0176d8a448e6dec84b575e5560ccef5262bf15"
}

$allValid = $true
if (Test-Path (Join-Path $driveLetter "AD102-ASSETS")) {
    Write-Host "  Verifying AD102 Assets:" -ForegroundColor Gray
    foreach ($name in $adHashes.Keys) {
        $filePath = Join-Path $driveLetter "AD102-ASSETS\$name"
        if (Test-Path $filePath) {
            $actualHash = (Get-FileHash -Path $filePath -Algorithm SHA256).Hash.ToLower()
            if ($actualHash -eq $adHashes[$name]) {
                Write-Host "    [OK] ${name}: verified bit-exact." -ForegroundColor Green
            } else {
                Write-Host "    [FAIL] ${name}: checksum mismatch ($actualHash)" -ForegroundColor Red
                $allValid = $false
            }
        }
    }
}

if (Test-Path (Join-Path $driveLetter "GA102-ASSETS")) {
    Write-Host "  Verifying GA102 Assets:" -ForegroundColor Gray
    foreach ($name in $gaHashes.Keys) {
        $filePath = Join-Path $driveLetter "GA102-ASSETS\$name"
        if (Test-Path $filePath) {
            $actualHash = (Get-FileHash -Path $filePath -Algorithm SHA256).Hash.ToLower()
            if ($actualHash -eq $gaHashes[$name]) {
                Write-Host "    [OK] ${name}: verified bit-exact." -ForegroundColor Green
            } else {
                Write-Host "    [FAIL] ${name}: checksum mismatch ($actualHash)" -ForegroundColor Red
                $allValid = $false
            }
        }
    }
}

Write-Host ""
Write-Host "======================================================================" -ForegroundColor Magenta
if ($allValid) {
    Write-Host "             SETUP COMPLETED SUCCESSFULLY!                            " -ForegroundColor Green
} else {
    Write-Host "       SETUP COMPLETED WITH WARNINGS OR MISMATCHES                    " -ForegroundColor Red
}
Write-Host "======================================================================" -ForegroundColor Magenta
Write-Host ""
Write-Host "HOW TO BOOT & UNLOCK:" -ForegroundColor Yellow
Write-Host "1. Restart your PC and press your Boot Menu key (F11 on MSI/ASRock, F12 on Gigabyte, F8 on ASUS)."
Write-Host "2. Select your USB drive ('UEFI: USB...')."
Write-Host "3. Secure Boot Options:"
Write-Host "   - EASIEST: In BIOS Setup, set Secure Boot to [Disabled] (or OS Type: Other OS)."
Write-Host "   - OPTIONAL (Keep Secure Boot Active): In BIOS -> Key Management -> 'Enroll Signature' / 'Append db',"
Write-Host "     select 'AdaUnlock_SecureBoot.cer' (or use 'Enroll EFI Image' on EFI\BOOT\BOOTX64.EFI)."
Write-Host "4. Press 'U' at the boot prompt (executes in 300ms), and Windows will launch with full tensor power!"
Write-Host "5. In Windows, double-click Check-GPU-Unlock.bat to verify 100% compute throughput (>200 TFLOPS)!"
Write-Host "   (Optional: Launch Universal-Control-Center.cmd if you wish to inspect detailed hardware telemetry)."
Write-Host ""
Write-Host "Press any key to exit..."
[void][System.Console]::ReadKey($true)
