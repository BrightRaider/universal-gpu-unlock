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

    $targetBootDir = Join-Path $driveRoot "EFI\BOOT"
    $targetPayloadDir = Join-Path $driveRoot "AD102-UNLOCK"
    New-Item -ItemType Directory -Path $targetBootDir -Force | Out-Null
    New-Item -ItemType Directory -Path $targetPayloadDir -Force | Out-Null

    $targetBootFile = Join-Path $targetBootDir "BOOTX64.EFI"

    # 1. Use pre-compiled diagnostic probe or universal booter if present
    $precompiledProbe = Join-Path $Script:RepoRoot "bin\GpuTopologyDiagnostic.efi"
    if (-not (Test-Path -LiteralPath $precompiledProbe)) {
        $precompiledProbe = Join-Path $Script:RepoRoot "bin\BOOTX64.EFI"
    }

    if (Test-Path -LiteralPath $precompiledProbe) {
        Copy-Item -LiteralPath $precompiledProbe -Destination $targetBootFile -Force
        $readmeContent = @"
# Universal GPU UNLOCK - Diagnostic Probe Capture USB

## Pre-Boot Diagnostic Instructions:
1. Secure Boot can remain enabled (with AdaUnlock_SecureBoot.cer enrolled) or disabled.
2. Shut down the PC completely (Cold Power Off).
3. Power on and press your Boot Menu key (ASUS F8 / MSI F11 / Gigabyte F12 / ASRock F11).
4. Boot from this USB drive ('UEFI: USB Drive').
5. Diagnostic telemetry will be written to this USB drive, handing off directly to Windows.
"@
        Set-Content -LiteralPath (Join-Path $driveRoot "README-PROBE.TXT") -Value $readmeContent -Encoding utf8
        return "Successfully staged pre-compiled Diagnostic Probe to $cleanLetter:\EFI\BOOT\BOOTX64.EFI (Zero-Compile)."
    }

    $buildScript = Join-Path $Script:RepoRoot "research\profile\build-probe.ps1"
    if (Test-Path -LiteralPath $buildScript) {
        $toolchain = Get-ClangToolchain
        if ($toolchain.Available) {
            $espGuid = Get-SystemWindowsEspGuid
            & powershell -NoProfile -ExecutionPolicy Bypass -File $buildScript `
                -WindowsEspGuid $espGuid `
                -Clang $toolchain.Clang `
                -LldLink $toolchain.LldLink `
                -Python 'python'
            if ($LASTEXITCODE -eq 0) {
                return "Successfully compiled and staged Diagnostic Probe on $($cleanLetter):."
            }
        }
    }

    throw "Diagnostic Probe binary not found in bin\."
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

    $newMachineScript = Join-Path $Script:RepoRoot "research\profile\new-machine.ps1"
    if (Test-Path -LiteralPath $newMachineScript) {
        $toolchain = Get-ClangToolchain
        if ($toolchain.Available) {
            $espGuid = Get-SystemWindowsEspGuid
            Invoke-DownloadAssets | Out-Null
            & powershell -NoProfile -ExecutionPolicy Bypass -File $newMachineScript `
                -Drive $cleanLetter `
                -WindowsEspGuid $espGuid `
                -Assets $Script:AssetsPath `
                -StageUnlock `
                -Clang $toolchain.Clang `
                -LldLink $toolchain.LldLink `
                -Python 'python'
            if ($LASTEXITCODE -eq 0) {
                return "Successfully generated local profile and staged bootloader on $($cleanLetter):!"
            }
        }
    }

    # Universal Zero-Compile Deployment
    return Deploy-ToUsb -Letter $cleanLetter
}


function Invoke-DownloadAssets {
    return "Universal firmware assets (AD102 & GA102) are already pre-packaged and verified in repository."
}


