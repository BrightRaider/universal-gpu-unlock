# Universal GPU Unlock - Boot Log Exporter (USB, Hidden GPU_UNLOCK & Internal ESP)
# SPDX-License-Identifier: GPL-2.0-only

[CmdletBinding()]
param(
    [string]$Destination = "",
    [int]$MaxFiles = 10,
    [switch]$Quiet,
    [switch]$Elevated
)

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Destination) {
    $Destination = Join-Path $repoRoot "Boot-Logs"
}

function Test-IsAdmin {
    try {
        return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator
        )
    } catch {
        return $false
    }
}

function Get-CandidateBootLogs {
    param([string[]]$ExtraRoots = @())

    $found = @()
    $subDirs = @("GPU-UNLOCK", "AD102-UNLOCK", "GA102-UNLOCK", "RTX3090-UNLOCK")

    $roots = New-Object System.Collections.Generic.List[object]

    # 1. Mounted drive letters (USB drives, normal partitions)
    foreach ($drv in [System.IO.DriveInfo]::GetDrives()) {
        if ($drv.DriveType -eq 'Fixed' -or $drv.DriveType -eq 'Removable') {
            if ($drv.IsReady) {
                $roots.Add([pscustomobject]@{
                    Root   = $drv.RootDirectory.FullName
                    Source = "Drive $($drv.Name.TrimEnd('\')) ($($drv.VolumeLabel))"
                }) | Out-Null
            }
        }
    }

    # 2. Hidden FAT32 volumes & ESP AccessPaths via Storage cmdlets
    try {
        $parts = Get-Partition -ErrorAction SilentlyContinue
        foreach ($p in $parts) {
            foreach ($ap in $p.AccessPaths) {
                if ($ap -and $ap.StartsWith('\\?\Volume{', [System.StringComparison]::OrdinalIgnoreCase)) {
                    $vol = $null
                    try { $vol = $p | Get-Volume -ErrorAction SilentlyContinue } catch {}
                    $lbl = if ($vol -and $vol.FileSystemLabel) { $vol.FileSystemLabel } elseif ($p.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}') { 'ESP' } else { 'Hidden' }
                    $fs = if ($vol -and $vol.FileSystem) { $vol.FileSystem } else { '' }
                    if ($fs -in @('', 'FAT32', 'FAT', 'exFAT')) {
                        $roots.Add([pscustomobject]@{
                            Root   = $ap
                            Source = "Hidden Volume ($lbl)"
                        }) | Out-Null
                    }
                }
            }
        }
    } catch {}

    foreach ($er in $ExtraRoots) {
        if ($er) {
            $roots.Add([pscustomobject]@{
                Root   = $er
                Source = "Mounted ESP ($er)"
            }) | Out-Null
        }
    }

    $seen = @{}
    foreach ($entry in $roots) {
        $r = $entry.Root
        foreach ($sub in $subDirs) {
            $dir = Join-Path $r $sub
            try {
                if ([System.IO.Directory]::Exists($dir)) {
                    $files = [System.IO.Directory]::GetFiles($dir, "RUN*.TXT")
                    $hasAttempt = [System.IO.File]::Exists((Join-Path $dir "ATTEMPT.FLG"))
                    foreach ($f in $files) {
                        $fi = New-Object System.IO.FileInfo($f)
                        $key = "$($fi.LastWriteTimeUtc.Ticks)|$($fi.Length)|$($fi.Name)"
                        if (-not $seen.ContainsKey($key)) {
                            $seen[$key] = $true
                            $found += [pscustomobject]@{
                                FullName     = $fi.FullName
                                Name         = $fi.Name
                                LastWriteUtc = $fi.LastWriteTimeUtc
                                Length       = $fi.Length
                                Source       = $entry.Source
                                Folder       = $dir
                                HasAttempt   = $hasAttempt
                            }
                        }
                    }
                }
            } catch {}
        }
    }

    return @($found | Sort-Object LastWriteUtc, Name -Descending)
}

function Copy-ExportLogs {
    param(
        [Parameter(Mandatory = $true)]$Logs,
        [Parameter(Mandatory = $true)][string]$DestDir,
        [int]$Limit
    )

    New-Item -ItemType Directory -Path $DestDir -Force | Out-Null
    $selected = @($Logs | Select-Object -First $Limit)
    $exportedPaths = @()
    foreach ($item in $selected) {
        $dst = Join-Path $DestDir $item.Name
        if ([System.IO.File]::Exists($dst)) {
            $srcInfo = New-Object System.IO.FileInfo($dst)
            if ($srcInfo.Length -ne $item.Length -or $srcInfo.LastWriteTimeUtc -ne $item.LastWriteUtc) {
                $stamp = $item.LastWriteUtc.ToString("yyyyMMdd_HHmmss")
                $base = [System.IO.Path]::GetFileNameWithoutExtension($item.Name)
                $ext = [System.IO.Path]::GetExtension($item.Name)
                $dst = Join-Path $DestDir "${base}_${stamp}${ext}"
            }
        }
        [System.IO.File]::Copy($item.FullName, $dst, $true)
        try {
            $fi = New-Object System.IO.FileInfo($dst)
            $fi.LastWriteTimeUtc = $item.LastWriteUtc
        } catch {}
        $exportedPaths += [pscustomobject]@{
            Path       = $dst
            Source     = $item.Source
            HasAttempt = $item.HasAttempt
            Folder     = $item.Folder
        }
    }
    return $exportedPaths
}

$logs = Get-CandidateBootLogs
$tempEspMount = $null

try {
    if ($logs.Count -eq 0 -and -not (Test-IsAdmin)) {
        if ($Quiet -and -not $Elevated) {
            exit 2
        }
        if (-not $Quiet) {
            Write-Host "====================================================================" -ForegroundColor Cyan
            Write-Host "  Requesting Administrator privileges to inspect EFI System Partition..." -ForegroundColor Yellow
            Write-Host "====================================================================" -ForegroundColor Cyan
        }
        $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Destination `"$Destination`" -MaxFiles $MaxFiles -Elevated"
        if ($Quiet) { $argList += " -Quiet" }
        $proc = Start-Process powershell.exe -ArgumentList $argList -Verb RunAs -PassThru -Wait
        exit $proc.ExitCode
    }

    if ($logs.Count -eq 0 -and (Test-IsAdmin)) {
        $used = @([System.IO.DriveInfo]::GetDrives() | ForEach-Object { $_.Name.Substring(0, 1).ToUpper() })
        $pick = @('Z','Y','X','W','V','U','T','S','R','Q','P','O','N','M') | Where-Object { $_ -notin $used } | Select-Object -First 1
        if ($pick) {
            $mountRoot = "${pick}:"
            & mountvol.exe $mountRoot /S >$null 2>&1
            if ($LASTEXITCODE -eq 0) {
                $tempEspMount = $mountRoot
                $logs = Get-CandidateBootLogs -ExtraRoots @("$mountRoot\")
            }
        }
    }

    if ($logs.Count -eq 0) {
        if (-not $Quiet) {
            Write-Host ""
            Write-Host "  [!] No Universal GPU Unlock RUN*.TXT logs were found on USB, GPU_UNLOCK, or ESP." -ForegroundColor Yellow
            Write-Host "      Boot once via UEFI ([U], [P], [I], or [S]) to generate a log." -ForegroundColor Gray
            Write-Host ""
            pause
        }
        exit 1
    }

    $exported = Copy-ExportLogs -Logs $logs -DestDir $Destination -Limit $MaxFiles
    $latest = $exported[0]

    $attemptDirs = @($logs | Where-Object { $_.HasAttempt } | Select-Object -ExpandProperty Folder -Unique)
    $notePath = Join-Path $Destination "BOOT_LOG_INFO.txt"
    $noteLines = @(
        "Latest exported log: $($latest.Path)",
        "Source: $($latest.Source)",
        "Exported files: $($exported.Count)",
        "ATTEMPT.FLG present: $($attemptDirs.Count -gt 0)"
    )
    [System.IO.File]::WriteAllLines($notePath, $noteLines, [System.Text.Encoding]::UTF8)

    if (-not $Quiet) {
        Write-Host ""
        Write-Host "====================================================================" -ForegroundColor Green
        Write-Host "  BOOT LOG EXPORTED SUCCESSFULLY" -ForegroundColor Green
        Write-Host "====================================================================" -ForegroundColor Green
        Write-Host "  Source      : $($latest.Source)" -ForegroundColor White
        Write-Host "  Latest File : $($latest.Path)" -ForegroundColor Cyan
        Write-Host "  Total Copied: $($exported.Count) log(s) into $Destination" -ForegroundColor Gray
        if ($attemptDirs.Count -gt 0) {
            Write-Host "  [!] ATTEMPT.FLG is currently present on the boot volume!" -ForegroundColor Yellow
        }
        Write-Host ""
        try {
            Start-Process explorer.exe -ArgumentList "/select,`"$($latest.Path)`"" | Out-Null
        } catch {}
        pause
    }
    exit 0
} finally {
    if ($tempEspMount) {
        & mountvol.exe $tempEspMount /D >$null 2>&1
    }
}
