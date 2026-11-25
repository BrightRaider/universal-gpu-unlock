# Universal GPU Unlock - Dedicated Internal 512 MB Partition Creator
# Author: BrightRaider
# SPDX-License-Identifier: GPL-2.0-only

[CmdletBinding()]
param(
    [string]$TargetDrive = $null,
    [switch]$Hidden,
    [switch]$NonInteractive
)

# 1. Require Administrator Privileges
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "====================================================================" -ForegroundColor Cyan
    Write-Host "  Requesting Administrator privileges..." -ForegroundColor Yellow
    Write-Host "====================================================================" -ForegroundColor Cyan
    $cmd = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    if ($TargetDrive) { $cmd += " -TargetDrive `"$TargetDrive`"" }
    if ($Hidden) { $cmd += " -Hidden" }
    if ($NonInteractive) { $cmd += " -NonInteractive" }
    Start-Process powershell -ArgumentList $cmd -Verb RunAs
    exit
}

Clear-Host
Write-Host "====================================================================" -ForegroundColor Cyan
Write-Host "  UNIVERSAL GPU UNLOCK - Dedicated Internal ESP Creator (512 MB)" -ForegroundColor White
Write-Host "====================================================================" -ForegroundColor Cyan
Write-Host "  Resolves the standard 100 MB Windows EFI partition space crunch for" -ForegroundColor Gray
Write-Host "  Option [I] (Stickless DXE Driver) on Ada (RTX 40) and Ampere (RTX 30)." -ForegroundColor Gray
Write-Host ""
Write-Host "  HOW IT WORKS:" -ForegroundColor Yellow
Write-Host "  1. Shrinks a selected internal SSD volume by 512 MB (leaving all data intact)." -ForegroundColor Gray
Write-Host "  2. Creates a dedicated 512 MB FAT32 partition labeled 'GPU_UNLOCK'." -ForegroundColor Gray
Write-Host "  3. UEFI BIOS automatically discovers and mounts this FAT32 partition on boot." -ForegroundColor Gray
Write-Host "  4. Option [I] in the bootloader automatically deploys AdaUnlockDxe.efi" -ForegroundColor Gray
Write-Host "     and NVIDIA GSP firmware assets (~85 MB) onto this partition." -ForegroundColor Gray
Write-Host "--------------------------------------------------------------------" -ForegroundColor DarkGray

# 2. Check for an existing GPU_UNLOCK partition
try {
    $existing = Get-Volume -ErrorAction SilentlyContinue | Where-Object { 
        ($_.FileSystemLabel -eq 'GPU_UNLOCK' -or $_.FileSystemLabel -eq 'UNLOCK_ESP') -and $_.FileSystem -eq 'FAT32' 
    } | Select-Object -First 1
    if ($existing) {
        $exMb = [math]::Round($existing.Size / 1MB)
        Write-Host ""
        Write-Host "  [OK] Dedicated unlock partition already exists:" -ForegroundColor Green
        Write-Host "       Drive: $($existing.DriveLetter): | Label: $($existing.FileSystemLabel) | Size: ${exMb} MB | FileSystem: FAT32" -ForegroundColor Green
        Write-Host ""
        Write-Host "  No additional partition needs to be created. Option [I] will use this drive!" -ForegroundColor Cyan
        Write-Host ""
        Write-Host "Press any key to exit..."
        if (-not $NonInteractive) { [void][System.Console]::ReadKey($true) }
        exit 0
    }
} catch { Write-Verbose "Scan existing: $($_.Exception.Message)" }

# 3. Detect candidate volumes for shrinking (Fixed NTFS/ReFS volumes with >= 1 GB free)
$candidates = @()
try {
    $vols = Get-Volume -ErrorAction SilentlyContinue | Where-Object { 
        $_.DriveType -eq 'Fixed' -and $_.DriveLetter -and ($_.FileSystem -eq 'NTFS' -or $_.FileSystem -eq 'ReFS') -and $_.SizeRemaining -ge 1073741824 
    }
    foreach ($v in $vols) {
        # Get partition info to verify disk style is GPT
        $part = Get-Partition -DriveLetter $v.DriveLetter -ErrorAction SilentlyContinue
        if ($part) {
            $disk = Get-Disk -Number $part.DiskNumber -ErrorAction SilentlyContinue
            if ($disk -and $disk.PartitionStyle -eq 'GPT') {
                $candidates += [PSCustomObject]@{
                    DriveLetter     = $v.DriveLetter
                    Label           = if ($v.FileSystemLabel) { $v.FileSystemLabel } else { "(No Label)" }
                    DiskNumber      = $part.DiskNumber
                    DiskName        = $disk.FriendlyName
                    PartitionNumber = $part.PartitionNumber
                    TotalGb         = [math]::Round($v.Size / 1GB, 1)
                    FreeGb          = [math]::Round($v.SizeRemaining / 1GB, 1)
                    CurrentPartSize = $part.Size
                    IsSystemDrive   = ($v.DriveLetter -eq 'C')
                }
            }
        }
    }
} catch {
    Write-Host "  [ERROR] Failed to query disk volumes: $($_.Exception.Message)" -ForegroundColor Red
}

if ($candidates.Count -eq 0) {
    Write-Host ""
    Write-Host "  [!] No suitable GPT volumes with at least 1 GB free space found." -ForegroundColor Yellow
    Write-Host "      You can manually create a 500 MB FAT32 partition using Windows Disk Management." -ForegroundColor DarkYellow
    Write-Host ""
    $openDm = Read-Host "  Would you like to open Windows Disk Management (diskmgmt.msc) now? (y/N)"
    if ($openDm -match '^[yYjJ]') {
        Start-Process diskmgmt.msc
    }
    exit 1
}

# Sort candidates: non-C: drives first (secondary SSDs are safest), then by free space
$sorted = $candidates | Sort-Object IsSystemDrive, FreeGb -Descending

Write-Host ""
Write-Host "  Available Internal Volumes with Free Space:" -ForegroundColor Cyan
for ($i = 0; $i -lt $sorted.Count; $i++) {
    $c = $sorted[$i]
    $tag = if ($c.IsSystemDrive) { " (System C:)" } else { " (Recommended - Data)" }
    Write-Host "    [$($i+1)] Drive $($c.DriveLetter): [$($c.Label)] - $($c.FreeGb) GB Free / $($c.TotalGb) GB Total ($($c.DiskName))$tag"
}
Write-Host "    [M] Open Windows Disk Management (diskmgmt.msc) for manual partitioning" -ForegroundColor Gray
Write-Host "    [Q] Cancel / Quit" -ForegroundColor Gray
Write-Host ""

$selectedCandidate = $null

if ($TargetDrive) {
    $match = $sorted | Where-Object { $_.DriveLetter -eq $TargetDrive.TrimEnd(':') } | Select-Object -First 1
    if ($match) { $selectedCandidate = $match }
}

if (-not $selectedCandidate -and -not $NonInteractive) {
    $choice = Read-Host "  Select volume to shrink by 512 MB [1-$($sorted.Count), M, Q]"
    if ($choice -match '^[qQ]') {
        Write-Host "  Aborted by user." -ForegroundColor Yellow
        exit 0
    }
    if ($choice -match '^[mM]') {
        Start-Process diskmgmt.msc
        exit 0
    }
    if ($choice -match '^\d+$' -and [int]$choice -ge 1 -and [int]$choice -le $sorted.Count) {
        $selectedCandidate = $sorted[[int]$choice - 1]
    }
}

if (-not $selectedCandidate) {
    Write-Host "  Invalid selection or target drive not found." -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "  Selected Volume: Drive $($selectedCandidate.DriveLetter): ($($selectedCandidate.Label), Disk $($selectedCandidate.DiskNumber))" -ForegroundColor Yellow
Write-Host "  Action: Shrink by 512 MB and create a dedicated FAT32 partition 'GPU_UNLOCK'." -ForegroundColor Yellow
Write-Host ""

if (-not $NonInteractive) {
    $confirm = Read-Host "  Proceed with creating the 512 MB partition on Disk $($selectedCandidate.DiskNumber)? (y/N)"
    if ($confirm -notmatch '^[yYjJ]') {
        Write-Host "  Operation cancelled." -ForegroundColor Yellow
        exit 0
    }
}

# 4. Perform Shrink & Partition Creation
$shrinkBytes = 536870912 # 512 MB
$targetLetter = $selectedCandidate.DriveLetter
$diskNum = $selectedCandidate.DiskNumber

Write-Host ""
Write-Host "  [1/3] Shrinking volume ${targetLetter}: by 512 MB..." -ForegroundColor Cyan

try {
    # Determine supported shrink range
    $supported = Get-PartitionSupportedSize -DriveLetter $targetLetter -ErrorAction Stop
    $targetSize = $selectedCandidate.CurrentPartSize - $shrinkBytes
    if ($targetSize -lt $supported.SizeMin) {
        throw "Volume ${targetLetter}: cannot be shrunk by 512 MB (unmovable files or SizeMin exceeded). Please use Disk Management (diskmgmt.msc)."
    }
    Resize-Partition -DriveLetter $targetLetter -Size $targetSize -ErrorAction Stop
    Write-Host "        [OK] Volume ${targetLetter}: successfully shrunk." -ForegroundColor Green
} catch {
    Write-Host "        [WARN] PowerShell Resize-Partition failed: $($_.Exception.Message)" -ForegroundColor Yellow
    Write-Host "        Attempting shrink via Windows diskpart..." -ForegroundColor Yellow
    
    $dpScript = @"
select disk $diskNum
select volume $targetLetter
shrink desired=512
"@
    $dpFile = [System.IO.Path]::GetTempFileName()
    Set-Content -Path $dpFile -Value $dpScript -Encoding ascii
    $dpOut = & diskpart.exe /s $dpFile
    Remove-Item $dpFile -Force -ErrorAction SilentlyContinue
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  [ERROR] diskpart shrink failed:`n$dpOut" -ForegroundColor Red
        Write-Host "  Opening Windows Disk Management (diskmgmt.msc)..." -ForegroundColor Yellow
        Start-Process diskmgmt.msc
        exit 1
    }
    Write-Host "        [OK] diskpart successfully shrunk volume ${targetLetter}:." -ForegroundColor Green
}

Write-Host "  [2/3] Creating new 512 MB partition on Disk $diskNum..." -ForegroundColor Cyan
$newPart = $null
try {
    $newPart = New-Partition -DiskNumber $diskNum -Size $shrinkBytes -ErrorAction Stop
    Write-Host "        [OK] New partition created (Partition $($newPart.PartitionNumber))." -ForegroundColor Green
} catch {
    Write-Host "        [WARN] PowerShell New-Partition failed, trying diskpart..." -ForegroundColor Yellow
    $dpScript = @"
select disk $diskNum
create partition primary size=512
"@
    $dpFile = [System.IO.Path]::GetTempFileName()
    Set-Content -Path $dpFile -Value $dpScript -Encoding ascii
    & diskpart.exe /s $dpFile | Out-Null
    Remove-Item $dpFile -Force -ErrorAction SilentlyContinue
    
    # Locate newly created partition
    Start-Sleep -Seconds 2
    $newPart = Get-Partition -DiskNumber $diskNum -ErrorAction SilentlyContinue | Sort-Object PartitionNumber -Descending | Select-Object -First 1
}

if (-not $newPart) {
    $newPart = Get-Partition -DiskNumber $diskNum -ErrorAction SilentlyContinue | Where-Object { -not $_.DriveLetter -and $_.Size -le ($shrinkBytes + 10485760) -and $_.Size -ge ($shrinkBytes - 10485760) } | Sort-Object PartitionNumber -Descending | Select-Object -First 1
}

if ($newPart) {
    # Prevent Windows Explorer from auto-mounting a raw drive letter and showing a premature format prompt.
    Set-Partition -DiskNumber $diskNum -PartitionNumber $newPart.PartitionNumber -NoDefaultDriveLetter $true -ErrorAction SilentlyContinue
}

Write-Host "  [3/3] Formatting partition with FAT32 ('GPU_UNLOCK')..." -ForegroundColor Cyan
$formatSuccess = $false
if ($newPart) {
    try {
        Format-Volume -Partition $newPart -FileSystem FAT32 -NewFileSystemLabel "GPU_UNLOCK" -Force -Confirm:$false -ErrorAction Stop | Out-Null
        $formatSuccess = $true
        Write-Host "        [OK] Partition $($newPart.PartitionNumber) formatted as FAT32 ('GPU_UNLOCK')." -ForegroundColor Green
    } catch {
        Write-Host "        [WARN] Format-Volume -Partition failed, falling back to temporary drive letter..." -ForegroundColor Yellow
        $usedLetters = (Get-Volume).DriveLetter | Where-Object { $_ }
        $candidateLetter = ('U', 'V', 'W', 'X', 'Y', 'Z', 'G', 'H', 'I', 'J') | Where-Object { $_ -notin $usedLetters } | Select-Object -First 1
        if (-not $candidateLetter) { $candidateLetter = 'U' }
        Set-Partition -DiskNumber $diskNum -PartitionNumber $newPart.PartitionNumber -NewDriveLetter $candidateLetter -ErrorAction SilentlyContinue
        $newPart = Get-Partition -DiskNumber $diskNum -PartitionNumber $newPart.PartitionNumber -ErrorAction SilentlyContinue
        if ($newPart -and $newPart.DriveLetter) {
            cmd.exe /c "format $($newPart.DriveLetter): /FS:FAT32 /V:GPU_UNLOCK /Q /Y" | Out-Null
            $formatSuccess = ($LASTEXITCODE -eq 0)
        }
    }
}

$hiddenSelected = [bool]$Hidden
if (-not $Hidden -and -not $NonInteractive -and $newPart) {
    $hideAns = Read-Host "  Hide drive letter from Windows Explorer (keep partition invisible, recommended)? (Y/n)"
    $hiddenSelected = ($hideAns -notmatch '^[nN]')
}

$hiddenApplied = $false
if ($formatSuccess -and $newPart) {
    if ($hiddenSelected) {
        try {
            Set-Partition -DiskNumber $diskNum -PartitionNumber $newPart.PartitionNumber -NoDefaultDriveLetter $true -ErrorAction Stop
            $newPart = Get-Partition -DiskNumber $diskNum -PartitionNumber $newPart.PartitionNumber -ErrorAction SilentlyContinue
            if ($newPart -and $newPart.DriveLetter) {
                Remove-PartitionAccessPath -DiskNumber $diskNum -PartitionNumber $newPart.PartitionNumber -AccessPath "$($newPart.DriveLetter):\" -ErrorAction Stop
            }
            $hiddenApplied = $true
            Write-Host "        [OK] NoDefaultDriveLetter set and no drive letter assigned (hidden in Explorer)." -ForegroundColor Green
        } catch {
            Write-Host "        [WARN] Could not hide drive letter automatically: $($_.Exception.Message)" -ForegroundColor Yellow
        }
    } else {
        try {
            Set-Partition -DiskNumber $diskNum -PartitionNumber $newPart.PartitionNumber -NoDefaultDriveLetter $false -ErrorAction SilentlyContinue
            if (-not $newPart.DriveLetter) {
                $usedLetters = (Get-Volume).DriveLetter | Where-Object { $_ }
                $candidateLetter = ('U', 'V', 'W', 'X', 'Y', 'Z', 'G', 'H', 'I', 'J') | Where-Object { $_ -notin $usedLetters } | Select-Object -First 1
                if ($candidateLetter) {
                    Set-Partition -DiskNumber $diskNum -PartitionNumber $newPart.PartitionNumber -NewDriveLetter $candidateLetter -ErrorAction SilentlyContinue
                }
            }
            $newPart = Get-Partition -DiskNumber $diskNum -PartitionNumber $newPart.PartitionNumber -ErrorAction SilentlyContinue
        } catch { }
    }
}

Write-Host ""
Write-Host "====================================================================" -ForegroundColor Cyan
if ($formatSuccess -and $newPart) {
    Write-Host "  SUCCESS: Dedicated 512 MB FAT32 Unlock Partition is Ready!" -ForegroundColor Green
    Write-Host "====================================================================" -ForegroundColor Cyan
    $driveDisplay = if ($hiddenApplied) { "(Hidden - No Drive Letter)" } else { "$($newPart.DriveLetter):" }
    Write-Host "  * Drive Letter:    $driveDisplay" -ForegroundColor White
    Write-Host "  * Volume Label:    GPU_UNLOCK" -ForegroundColor White
    Write-Host "  * File System:     FAT32 (UEFI Boot Compliant)" -ForegroundColor White
    Write-Host "  * Physical Disk:   Disk $diskNum ($($selectedCandidate.DiskName))" -ForegroundColor White
    Write-Host ""
    Write-Host "  NEXT STEPS FOR STICKLESS PERSISTENCE:" -ForegroundColor Yellow
    Write-Host "  1. Reboot your PC and boot from your Universal GPU Unlock USB stick." -ForegroundColor Gray
    Write-Host "  2. At the boot prompt, select Option [I] (Install to NVRAM)." -ForegroundColor Gray
    Write-Host "  3. The bootloader will automatically detect this 512 MB FAT32 partition" -ForegroundColor Gray
    Write-Host "     and install AdaUnlockDxe.efi and all active firmware assets onto it!" -ForegroundColor Gray
    Write-Host "  4. You can now remove the USB stick. Your system will unlock on every boot!" -ForegroundColor Gray
} else {
    Write-Host "  PARTITION CREATION PARTIALLY COMPLETED" -ForegroundColor Yellow
    Write-Host "====================================================================" -ForegroundColor Cyan
    Write-Host "  The partition was created but formatting may require manual completion." -ForegroundColor Yellow
    Write-Host "  Opening Windows Disk Management (diskmgmt.msc)..." -ForegroundColor Yellow
    Start-Process diskmgmt.msc
}

Write-Host ""
Write-Host "Press any key to exit..."
if (-not $NonInteractive) { [void][System.Console]::ReadKey($true) }
