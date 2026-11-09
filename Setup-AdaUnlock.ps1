<#
.SYNOPSIS
    Universal RTX 40 & RTX 30 Series Tensor Unlock Setup Tool (v1.6.3)
.DESCRIPTION
    Automated 1-Click Setup Tool to create a bootable UEFI flash drive
    with the universal Ada Lovelace & Ampere rate-limiter unlock.
#>

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
    $_.Name -like "*4090*" -or $_.Name -like "*4080*" -or $_.Name -like "*4070*" -or $_.Name -like "*4060*" -or
    $_.Name -like "*3090*" -or $_.Name -like "*3080*" -or $_.Name -like "*3070*" -or $_.Name -like "*3060*" -or $_.Name -like "*3050*" -or
    $_.Name -like "*2080*" -or $_.Name -like "*2070*" -or $_.Name -like "*2060*" -or $_.Name -like "*TITAN RTX*" -or $_.Name -like "*CMP 50HX*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2684*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_2685*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2704*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_2703*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2705*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_2782*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2783*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_2786*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2860*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_2882*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2204*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_2208*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2206*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_220A*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2216*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2482*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_2484*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2486*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_2487*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2501*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_2503*" -or
    $_.PNPDeviceID -like "*VEN_10DE&DEV_2504*" -or $_.PNPDeviceID -like "*VEN_10DE&DEV_2582*" -or
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
. (Join-Path (Join-Path $scriptDir "tools") "Deploy-UsbCommon.ps1")

# Bootloader S3 Menu Customization
Write-Host ""
Write-Host "  OPTIONAL: Bootloader Menu Customization" -ForegroundColor Gray
$s3Answer = Read-Host "  Hide experimental S3 Sleep Persistence [P] from boot menu? (y/N - default: No)"
$hideS3 = ($s3Answer -match "^[yYjJ]")

$deployResult = Deploy-UniversalToUsb -Drive $driveLetter -RepoRoot $scriptDir -HideS3 $hideS3 -AppVersion $AppVersion -OnLog {
    param($msg, $color)
    if ($color) { Write-Host $msg -ForegroundColor $color } else { Write-Host $msg }
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

$tuHashes = @{
    "booter_load_tu102.bin" = "3ae2cccc2666e5de1fee0db68a41ba37086124425eaed4d90718e194d5d48276"
    "v67-tu102-610.43.03.bin" = "2a0aa1f86d0b6afff5214d641d87d46dc98ef06ed908ff05d0ddbad5ffb7e197"
    "gsp_tu10x.bin" = "73065619db9ec921d19fc4e519dd04d91a9199b525eaca9b257b89fb8c5ec52c"
    "gsp_boot_tu102.bin" = "8765694cd38beaa4e2b8b1b83a31fb7aee6cb9d8b7c6bc7a86e6d931a3a056c2"
    "gsp_boot_desc_tu102.bin" = "9e7cca2cfdc5ce601b0adba39967850046108df6c2d23fff95578ca3f0ccbe01"
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

if (Test-Path (Join-Path $driveLetter "TU102-ASSETS")) {
    Write-Host "  Verifying TU102 Assets:" -ForegroundColor Gray
    foreach ($name in $tuHashes.Keys) {
        $filePath = Join-Path $driveLetter "TU102-ASSETS\$name"
        if (Test-Path $filePath) {
            $actualHash = (Get-FileHash -Path $filePath -Algorithm SHA256).Hash.ToLower()
            if ($actualHash -eq $tuHashes[$name]) {
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
