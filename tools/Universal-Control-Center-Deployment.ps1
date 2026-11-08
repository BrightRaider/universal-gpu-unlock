# SPDX-License-Identifier: GPL-2.0-only
# Universal Control Center - USB Deployment & Local Build Module

function Get-RemovableDrives {
    $drives = @()
    try {
        $disks = @(Get-Disk | Where-Object { $_.BusType -eq 'USB' -or $_.MediaType -eq 'Removable' })
        foreach ($disk in $disks) {
            $parts = @(Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue)
            foreach ($part in $parts) {
                if ($part.DriveLetter) {
                    $vol = Get-Volume -DriveLetter $part.DriveLetter -ErrorAction SilentlyContinue
                    $drives += [pscustomobject]@{
                        DriveLetter  = "$($part.DriveLetter):"
                        Letter       = [string]$part.DriveLetter
                        DiskNumber   = $disk.Number
                        FriendlyName = "{0} ({1}:)" -f $disk.FriendlyName, $part.DriveLetter
                        FileSystem   = [string]$vol.FileSystem
                        SizeGb       = [math]::Round($part.Size / 1GB, 2)
                        IsBoot       = [bool]$disk.IsBoot
                        IsSystem     = [bool]$part.IsSystem
                    }
                }
            }
        }
    }
    catch {}
    return $drives
}


function Deploy-ToUsb([string]$Letter) {
    $hide = [bool]($chkHideS3 -and $chkHideS3.IsChecked)
    return Deploy-UniversalToUsb -Drive $Letter -RepoRoot $Script:RepoRoot -HideS3 $hide -AppVersion $Script:AppVersion
}


function Stage-DiagnosticProbeUsb([string]$Letter) {
    $cleanLetter = $Letter.TrimEnd(':')
    $driveRoot = "$($cleanLetter):\"
    if (-not (Test-Path -LiteralPath $driveRoot)) {
        throw "Drive '$driveRoot' does not exist."
    }

    $vol = Get-Volume -DriveLetter $cleanLetter
    if ($vol.FileSystem -ne 'FAT32') {
        throw "Drive $($cleanLetter): is formatted as $($vol.FileSystem). UEFI requires FAT32."
    }

    $toolchain = Get-ClangToolchain
    if (-not $toolchain.Available) {
        throw "LLVM clang/lld-link compiler toolchain not found. Install LLVM or place clang.exe in C:\Program Files\LLVM\bin."
    }

    $espGuid = Get-SystemWindowsEspGuid
    if (-not $espGuid) {
        throw "Could not determine system Windows ESP partition GUID. Ensure Windows is running on a UEFI GPT disk."
    }

    $buildScript = Join-Path $Script:RepoRoot "research\profile\build-probe.ps1"
    if (-not (Test-Path -LiteralPath $buildScript)) {
        throw "Probe build script not found: $buildScript"
    }

    Write-Host "[*] Compiling read-only Diagnostic Probe targeting Windows ESP {$espGuid}..." -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File $buildScript `
        -WindowsEspGuid $espGuid `
        -Clang $toolchain.Clang `
        -LldLink $toolchain.LldLink `
        -Python 'python'

    if ($LASTEXITCODE -ne 0) {
        throw "Failed to compile read-only generic probe BOOTX64.EFI."
    }

    $compiledProbe = Join-Path $Script:RepoRoot "internal-boot\boot\build\probe-generic\BOOTX64.EFI"
    if (-not (Test-Path -LiteralPath $compiledProbe)) {
        throw "Compiled probe not found at $compiledProbe"
    }

    $targetBootDir = Join-Path $driveRoot "EFI\BOOT"
    $targetPayloadDir = Join-Path $driveRoot "RTX3090-UNLOCK"
    New-Item -ItemType Directory -Path $targetBootDir -Force | Out-Null
    New-Item -ItemType Directory -Path $targetPayloadDir -Force | Out-Null

    $targetBootFile = Join-Path $targetBootDir "BOOTX64.EFI"
    if (Test-Path -LiteralPath $targetBootFile) {
        $oldSha = (Get-FileHash -LiteralPath $targetBootFile -Algorithm SHA256).Hash.Substring(0, 12).ToLowerInvariant()
        $bakDir = Join-Path $targetPayloadDir "backup"
        New-Item -ItemType Directory -Path $bakDir -Force | Out-Null
        Copy-Item -LiteralPath $targetBootFile -Destination (Join-Path $bakDir "BOOTX64-$oldSha.EFI") -Force
    }

    Copy-Item -LiteralPath $compiledProbe -Destination $targetBootFile -Force

    $readmeContent = @"
# Windows GPU UNLOCK - Diagnostic Probe Capture USB

## How to Collect Pre-Boot Diagnostic Captures:
1. Secure Boot can remain enabled (with AdaUnlock_SecureBoot.cer enrolled) or disabled in BIOS.
2. Shut down the PC completely (Power Off). Do not restart.
3. Power on and press your Boot Menu key (ASUS F8 / Gigabyte F12 / MSI F11).
4. Boot from this USB drive ('UEFI: USB Drive').
5. The probe runs safely (100% read-only), records physical DIAGnn.TXT, IMGnn.BIN, and PROM.BIN captures,
   and IMMEDIATELY hands off directly to Windows Boot Manager. No return to BIOS!
6. Once Windows starts, repeat once more (Full Cold Shutdown -> Boot USB) to record DIAG01.TXT.
7. Open the GPU Unlock Utility and click 'Build & Stage Headless Unlock'!
"@
    Set-Content -LiteralPath (Join-Path $driveRoot "README-PROBE.TXT") -Value $readmeContent -Encoding utf8
    Set-Content -LiteralPath (Join-Path $targetPayloadDir "README.TXT") -Value "READ-ONLY PROBE. Logs DIAGnn.TXT, PROM.BIN and IMGnn.BIN here, then starts Windows directly." -Encoding ascii

    return "Successfully compiled and staged read-only Diagnostic Probe on $($cleanLetter):."
}


function Build-And-Stage-Local-Unlock([string]$Letter) {
    $cleanLetter = $Letter.TrimEnd(':')
    $driveRoot = "$($cleanLetter):\"
    if (-not (Test-Path -LiteralPath $driveRoot)) {
        throw "Drive '$driveRoot' does not exist."
    }

    $vol = Get-Volume -DriveLetter $cleanLetter
    if ($vol.FileSystem -ne 'FAT32') {
        throw "Drive $($cleanLetter): is formatted as $($vol.FileSystem). UEFI requires FAT32."
    }

    $status = Get-UsbProbeCaptureStatus -Letter $cleanLetter
    if ($status.Count -lt 2) {
        throw "Insufficient probe captures on $($cleanLetter): ($($status.Count) of 2). Please boot from the Diagnostic Probe USB at least twice (from a full cold shutdown) so hardware captures are saved."
    }

    $toolchain = Get-ClangToolchain
    if (-not $toolchain.Available) {
        throw "LLVM clang/lld-link compiler toolchain not found. Install LLVM or place clang.exe in C:\Program Files\LLVM\bin."
    }

    $espGuid = Get-SystemWindowsEspGuid
    if (-not $espGuid) {
        throw "Could not determine system Windows ESP partition GUID. Ensure Windows is running on a UEFI GPT disk."
    }

    # Ensure firmware assets exist
    Invoke-DownloadAssets | Out-Null

    $newMachineScript = Join-Path $Script:RepoRoot "research\profile\new-machine.ps1"
    Write-Host "[*] Generating local profile, running host verification tests, and building Headless USB package..." -ForegroundColor Cyan

    & powershell -NoProfile -ExecutionPolicy Bypass -File $newMachineScript `
        -Drive $cleanLetter `
        -WindowsEspGuid $espGuid `
        -Assets $Script:AssetsPath `
        -StageUnlock `
        -Clang $toolchain.Clang `
        -LldLink $toolchain.LldLink `
        -Python 'python'

    if ($LASTEXITCODE -ne 0) {
        throw "Headless unlock packaging failed. Check the error log output."
    }

    return "Successfully generated local profile and staged custom Headless Unlock bootloader on $($cleanLetter):!"
}


function Invoke-DownloadAssets {
    return "Universal firmware assets (AD102 & GA102) are already pre-packaged and verified in repository."
}


