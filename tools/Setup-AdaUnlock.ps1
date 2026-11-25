<#
.SYNOPSIS
    Universal RTX 40, 30 & 20 Series Tensor Unlock Setup Tool (version from src\version.h)
.DESCRIPTION
    Automated 1-Click Setup Tool to create a bootable UEFI flash drive
    with the universal Ada Lovelace, Ampere & Turing rate-limiter unlock.
#>

# 1. Elevate Administrator Privileges
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "[INFO] Requesting Administrator privileges..." -ForegroundColor Cyan
    Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit
}

# 0. Load the shared USB deployment module and the central version
$toolsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = if ((Split-Path -Leaf $toolsDir) -eq 'tools') { Split-Path -Parent $toolsDir } else { $toolsDir }
$deployModule = if (Test-Path -LiteralPath (Join-Path $toolsDir "Deploy-UsbCommon.ps1")) {
    Join-Path $toolsDir "Deploy-UsbCommon.ps1"
} else {
    Join-Path (Join-Path $repoRoot "tools") "Deploy-UsbCommon.ps1"
}
. $deployModule
$AppVersion = Get-GuVersion $repoRoot

Clear-Host
Write-Host "======================================================================" -ForegroundColor Magenta
Write-Host "   RTX 40, 30 & 20 SERIES UNIVERSAL UNLOCK - 1-CLICK SETUP ($AppVersion)      " -ForegroundColor Yellow
Write-Host "   Supported: Ada (4090..4060) | Ampere (3090..3050) | Turing (2080Ti..2060)" -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Magenta
Write-Host ""

# 2. Hardware Detection (GPU) & Pre-Flight Safety Checks
Write-Host "[1/5] Scanning for compatible NVIDIA GeForce GPU (Ada / Ampere / Turing)..." -ForegroundColor Cyan
$supportedIds = Get-GuSupportedGpuIds $repoRoot
$gpus = Get-CimInstance Win32_VideoController | Where-Object {
    $_.PNPDeviceID -match 'VEN_10DE&DEV_([0-9A-Fa-f]{4})' -and $supportedIds.ContainsKey($matches[1].ToLowerInvariant())
}

if ($gpus) {
    foreach ($gpu in $gpus) {
        Write-Host "  [OK] Detected target GPU: $($gpu.Name)" -ForegroundColor Green
    }
} else {
    Write-Host "  [NOTE] Target GPU not found in current device list (Setup can still proceed)." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "[2/5] Pre-Flight Safety Checks (ESP, BitLocker, Secure Boot, ACPI S3)..." -ForegroundColor Cyan

# 2a. Check Windows EFI System Partition (ESP) size
try {
    $esp = Get-Partition -ErrorAction SilentlyContinue | Where-Object { $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' } | Select-Object -First 1
    if ($esp) {
        $espMb = [math]::Round($esp.Size / 1MB)
        if ($espMb -lt 250) {
            Write-Host "  [!] Windows EFI Partition is ${espMb} MB (standard 100 MB OEM ESP)." -ForegroundColor Yellow
            Write-Host "      Option [I] requires ~85 MB free for Ada/Ampere assets (~28 MB for Turing)." -ForegroundColor DarkYellow
            Write-Host "      Tip: Use Option [P] (S3 Sleep) or Setup.bat [3] (hidden 512 MB GPU_UNLOCK partition)." -ForegroundColor DarkYellow
        } else {
            Write-Host "  [OK] Windows EFI Partition: ${espMb} MB (sufficient space for stickless [I] driver)." -ForegroundColor Green
        }
    }
} catch { Write-Verbose "ESP check skipped: $($_.Exception.Message)" }

# 2b. BitLocker Check on C:
try {
    $bl = Get-BitLockerVolume -MountPoint C: -ErrorAction SilentlyContinue
    if ($bl -and ($bl.ProtectionStatus -eq 1 -or $bl.VolumeStatus -ne 'FullyDecrypted')) {
        Write-Host "  [!] WARNING: BitLocker protection is ACTIVE on C: ($($bl.ProtectionStatus))." -ForegroundColor Yellow
        Write-Host "      Suspend BitLocker (or have your 48-digit recovery key ready) before changing UEFI boot." -ForegroundColor DarkYellow
    } else {
        Write-Host "  [OK] BitLocker on C:: Not active." -ForegroundColor Green
    }
} catch {
    Write-Host "  [i] BitLocker check skipped (unavailable on this edition)." -ForegroundColor DarkGray
}

# 2c. Secure Boot Status
try {
    $sbEnabled = $null
    $sbReg = Get-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State" -Name UEFISecureBootEnabled -ErrorAction SilentlyContinue
    if ($sbReg) { $sbEnabled = ($sbReg.UEFISecureBootEnabled -eq 1) }
    if ($null -eq $sbEnabled) { $sbEnabled = Confirm-SecureBootUEFI }
    if ($sbEnabled -eq $true) {
        Write-Host "  [i] Secure Boot: ENABLED (Append 'AdaUnlock_SecureBoot.cer' in BIOS db or disable Secure Boot)." -ForegroundColor Cyan
    } else {
        Write-Host "  [OK] Secure Boot: Disabled (custom UEFI binaries boot directly)." -ForegroundColor Green
    }
} catch {
    Write-Host "  [i] Secure Boot status: Not detected." -ForegroundColor DarkGray
}

# 2d. ACPI S3 vs Modern Standby (S0ix) Check via Kernel FADT Flags
try {
    $fadtKey = Get-ChildItem "HKLM:\HARDWARE\ACPI\FADT" -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.Property -contains '00000000' } | Select-Object -First 1
    $s0ix = $false
    if ($fadtKey) {
        $fadtBytes = (Get-ItemProperty -LiteralPath $fadtKey.PSPath -Name '00000000' -ErrorAction SilentlyContinue).'00000000'
        if ($fadtBytes -and $fadtBytes.Length -ge 116) {
            $flags = [BitConverter]::ToUInt32($fadtBytes, 112)
            $s0ix = (($flags -band (1 -shl 21)) -ne 0)
        }
    }
    if ($s0ix) {
        Write-Host "  [!] Standby Check: Modern Standby (S0ix) detected in ACPI FADT." -ForegroundColor Yellow
        Write-Host "      Option [P] (S3 Persistence) requires classic ACPI S3 enabled in BIOS." -ForegroundColor DarkYellow
    } else {
        Write-Host "  [OK] Standby Check: Classic ACPI S3 platform (Option [P] supported)." -ForegroundColor Green
    }
} catch { Write-Verbose "S0ix check skipped: $($_.Exception.Message)" }

Write-Host ""
Write-Host "[3/5] Scanning for USB Flash Drives..." -ForegroundColor Cyan

# 3. Comprehensive USB Drive Detection
$detectedDrives = @()
try {
    $removableVols = Get-CimInstance Win32_Volume -ErrorAction SilentlyContinue | Where-Object { $_.DriveType -eq 2 -and $_.DriveLetter }
    if ($removableVols) { $detectedDrives += $removableVols }
} catch { Write-Verbose "Ignored: $($_.Exception.Message)" }

try {
    $usbDisks = Get-Disk -ErrorAction SilentlyContinue | Where-Object { $_.BusType -eq 'USB' }
    foreach ($d in $usbDisks) {
        $parts = $d | Get-Partition -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter }
        foreach ($p in $parts) {
            $vol = Get-Volume -DriveLetter $p.DriveLetter -ErrorAction SilentlyContinue
            if ($vol -and ($detectedDrives.DriveLetter -notcontains ($vol.DriveLetter + ":"))) {
                $detectedDrives += [PSCustomObject]@{
                    DriveLetter = ($vol.DriveLetter + ":")
                    Label       = $vol.FileSystemLabel
                    Capacity    = $vol.Size
                }
            }
        }
    }
} catch { Write-Verbose "Ignored: $($_.Exception.Message)" }

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
Write-Host "[4/5] Copying Universal Bootloader & Firmware Assets (Ada, Ampere & Turing)..." -ForegroundColor Cyan

Write-Host ""
Write-Host "  OPTIONAL: Bootloader Menu Customization" -ForegroundColor Gray
$s3Answer = Read-Host "  Hide S3 Sleep Persistence [P] from boot menu? (y/N - default: No)"
$hideS3 = ($s3Answer -match "^[yYjJ]")

$deployResult = Deploy-UniversalToUsb -Drive $driveLetter -RepoRoot $repoRoot -HideS3 $hideS3 -AppVersion $AppVersion -OnLog {
    param($msg, $color)
    if ($color) { Write-Host $msg -ForegroundColor $color } else { Write-Host $msg }
}

# 5. Integrity Verification (SHA-256)
Write-Host ""
Write-Host "[5/5] Verifying bit-exact SHA-256 checksums..." -ForegroundColor Cyan

$adHashes = @{
    "booter_load_ad102.bin"             = "6ee05b20e1dc244c08c052f36ef28469b8d09f4955d1fe33e48a179e11a7fd49"
    "v67-ad102-610.43.03-four-plm.bin"  = "44c36b2d6b3424e53e18f3e55db85c833655de071ce66ce42749588a4a7dc8cb"
    "gsp_ga10x.bin"                     = "572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a"
    "gsp_boot_ad102.bin"                = "45d6335a8185d6ff13e8d1484cea73f68d3d13aa1c19dc080ed1c3482dd5bac0"
    "gsp_boot_desc_ad102.bin"           = "83f8f3f1f5f9496e0ea8fc7103c2beb64d0b6de2f5ffe725f13514de5a95fa42"
}

$gaHashes = @{
    "booter_load_ga102.bin"   = "056cbc064a888f80717726c54d7f64fcd68044d123870c95a74f37f4975dcaa4"
    "v67-ga102-610.43.03.bin" = "2a0aa1f86d0b6afff5214d641d87d46dc98ef06ed908ff05d0ddbad5ffb7e197"
    "gsp_ga10x.bin"           = "572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a"
    "gsp_boot_ga102.bin"      = "3df651acb174fd5f5561a1996965b192f50c5d763168446ed0d4ead19d895768"
    "gsp_boot_desc_ga102.bin" = "cc5a3fc1f4c47f2fe107b1b68f0176d8a448e6dec84b575e5560ccef5262bf15"
}

$tuHashes = @{
    "booter_load_tu102.bin"   = "3ae2cccc2666e5de1fee0db68a41ba37086124425eaed4d90718e194d5d48276"
    "v67-tu102-610.43.03.bin" = "2a0aa1f86d0b6afff5214d641d87d46dc98ef06ed908ff05d0ddbad5ffb7e197"
    "gsp_tu10x.bin"           = "73065619db9ec921d19fc4e519dd04d91a9199b525eaca9b257b89fb8c5ec52c"
    "gsp_boot_tu102.bin"      = "8765694cd38beaa4e2b8b1b83a31fb7aee6cb9d8b7c6bc7a86e6d931a3a056c2"
    "gsp_boot_desc_tu102.bin" = "9e7cca2cfdc5ce601b0adba39967850046108df6c2d23fff95578ca3f0ccbe01"
}

$allValid = $true
foreach ($pair in @(
    @{ Folder = "AD102-ASSETS"; Label = "AD102"; Hashes = $adHashes },
    @{ Folder = "GA102-ASSETS"; Label = "GA102"; Hashes = $gaHashes },
    @{ Folder = "TU102-ASSETS"; Label = "TU102"; Hashes = $tuHashes }
)) {
    $dirPath = Join-Path $driveLetter $pair.Folder
    if (Test-Path $dirPath) {
        Write-Host "  Verifying $($pair.Label) Assets:" -ForegroundColor Gray
        foreach ($name in $pair.Hashes.Keys) {
            $filePath = Join-Path $dirPath $name
            if (Test-Path $filePath) {
                $actualHash = (Get-FileHash -Path $filePath -Algorithm SHA256).Hash.ToLower()
                if ($actualHash -eq $pair.Hashes[$name]) {
                    Write-Host "    [OK] ${name}: verified bit-exact." -ForegroundColor Green
                } else {
                    Write-Host "    [FAIL] ${name}: checksum mismatch ($actualHash)" -ForegroundColor Red
                    $allValid = $false
                }
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
Write-Host "3. BIOS Setup Prerequisites:"
Write-Host "   - REQUIRED: Set CSM (Compatibility Support Module) to [Disabled] (pure UEFI mode)."
Write-Host "   - PRO-TIP: Enable 'Above 4G Decoding' and 'Re-Size BAR Support' for extra AI & gaming speed!"
Write-Host "   - SECURE BOOT (Append Key): In BIOS -> Key Management -> 'Append db', select 'AdaUnlock_SecureBoot.cer'."
Write-Host "4. Choose [U] (one-shot), [P] (S3 Sleep Persistence), or [I] (Stickless NVRAM DXE Driver)."
Write-Host "5. In Windows, run Launch-Dashboard.bat (GUI Cockpit) or Check-GPU-Unlock.bat (CLI Benchmark)!"
Write-Host "6. Note for 100 MB Windows ESPs: Run Setup.bat [3] (or tools\Create-Unlock-Partition.bat) to create"
Write-Host "   a hidden 512 MB GPU_UNLOCK partition for stickless [I] + [P] (including Riot Vanguard / Secure Boot)."
Write-Host ""
Write-Host "Press any key to exit..."
[void][System.Console]::ReadKey($true)
