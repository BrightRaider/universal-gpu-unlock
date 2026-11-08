# SPDX-License-Identifier: GPL-2.0-only
# Universal Control Center - Diagnostic Dumps & Compatibility Analysis Module

function Get-ClangToolchain {
    $clangPath = $null
    $lldLinkPath = $null

    $candidates = @(
        "C:\Program Files\LLVM\bin",
        "C:\TOOLS\LLVM20\bin",
        "C:\TOOLS\LLVM\bin"
    )
    foreach ($c in $candidates) {
        $cClang = Join-Path $c "clang.exe"
        $cLld = Join-Path $c "lld-link.exe"
        if ((Test-Path -LiteralPath $cClang) -and (Test-Path -LiteralPath $cLld)) {
            $clangPath = $cClang
            $lldLinkPath = $cLld
            break
        }
    }

    if (-not $clangPath) {
        $cmdClang = Get-Command clang.exe -ErrorAction SilentlyContinue
        $cmdLld = Get-Command lld-link.exe -ErrorAction SilentlyContinue
        if ($cmdClang -and $cmdLld) {
            $clangPath = $cmdClang.Source
            $lldLinkPath = $cmdLld.Source
        }
    }

    return @{
        Available = (-not [string]::IsNullOrEmpty($clangPath))
        Clang     = $clangPath
        LldLink   = $lldLinkPath
    }
}


function Get-UsbProbeCaptureStatus([string]$Letter) {
    $cleanLetter = $Letter.TrimEnd(':')
    $dir = "$($cleanLetter):\RTX3090-UNLOCK"
    $captures = @()
    if (Test-Path -LiteralPath $dir) {
        $files = Get-ChildItem -LiteralPath $dir -Filter 'DIAG??.TXT' -File -ErrorAction SilentlyContinue
        foreach ($f in $files) {
            try {
                $t = [IO.File]::ReadAllText($f.FullName)
                if ($t.Contains('probe-pci-handles=') -and $t.Contains("capture-complete=1`r`n")) {
                    $captures += $f
                }
            } catch {}
        }
    }
    $hasProm = Test-Path -LiteralPath (Join-Path $dir 'PROM.BIN')
    $hasImg = (Test-Path -LiteralPath (Join-Path $dir 'IMG00.BIN')) -or (Test-Path -LiteralPath (Join-Path $dir 'IMG01.BIN'))
    return @{
        Count     = $captures.Count
        Required  = 2
        Captures  = $captures
        HasProm   = $hasProm
        HasImg    = $hasImg
        Directory = $dir
    }
}


function Find-UsbCaptureFolders {
    $found = @()
    $drives = Get-RemovableDrives
    foreach ($d in $drives) {
        $root = "$($d.Letter):\"
        # Check root
        $diagRoot = @(Get-ChildItem -LiteralPath $root -Filter "DIAG*.TXT" -File -ErrorAction SilentlyContinue)
        if ($diagRoot.Count -gt 0) {
            $found += $root.TrimEnd('\')
        }
        # Check RTX3090-UNLOCK
        $rtxDir = Join-Path $root "RTX3090-UNLOCK"
        if (Test-Path -LiteralPath $rtxDir) {
            $found += $rtxDir
        }
    }
    return @($found | Select-Object -Unique)
}


function Export-WindowsDiagnosticDump([string]$TargetDir) {
    if (-not $TargetDir) { $TargetDir = Join-Path $Script:RepoRoot "diagnostics" }
    if (-not (Test-Path -LiteralPath $TargetDir)) {
        New-Item -ItemType Directory -Path $TargetDir -Force | Out-Null
    }

    $timestamp = [DateTime]::Now.ToString("yyyyMMdd-HHmmss")
    $dumpFolder = Join-Path $TargetDir "GPU-UNLOCK-DIAG-$timestamp"
    New-Item -ItemType Directory -Path $dumpFolder -Force | Out-Null

    # 1. System and GPU Inventory JSON
    $invScript = Join-Path $Script:RepoRoot "tools\collect_compatibility.py"
    if (Test-Path -LiteralPath $invScript) {
        $invJson = & python -I $invScript 2>&1
        $invJson | Set-Content -LiteralPath (Join-Path $dumpFolder "WINDOWS-INVENTORY.JSON") -Encoding utf8
    }

    # 2. NVIDIA-SMI XML & Tabular Query
    try {
        $smiXml = & nvidia-smi -q -x 2>&1
        $smiXml | Set-Content -LiteralPath (Join-Path $dumpFolder "NVIDIA-SMI-DUMP.XML") -Encoding utf8
    }
    catch {}

    # 3. System & Pre-Flight Summary
    $audit = Get-GpuHardwareInfo
    $preflight = Get-PreflightChecks
    $summary = [ordered]@{
        GeneratedUtc = [DateTime]::UtcNow.ToString("o")
        Audit        = $audit
        Preflight    = $preflight
    }
    $summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $dumpFolder "SYSTEM-SUMMARY.JSON") -Encoding utf8

    # 4. Display & EDID Registry Dump
    try {
        $displayReg = "HKLM:\SYSTEM\CurrentControlSet\Enum\DISPLAY"
        $edidEntries = @()
        if (Test-Path -LiteralPath $displayReg) {
            $monitors = Get-ChildItem -LiteralPath $displayReg -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -eq 'Device Parameters' }
            foreach ($m in $monitors) {
                $edidBytes = (Get-ItemProperty -LiteralPath $m.PSPath -Name "EDID" -ErrorAction SilentlyContinue).EDID
                if ($edidBytes) {
                    $edidHex = ($edidBytes | ForEach-Object { '{0:x2}' -f $_ }) -join ''
                    $edidEntries += [ordered]@{
                        Path     = $m.PSPath
                        EDID_Hex = $edidHex
                        Bytes    = $edidBytes.Length
                    }
                }
            }
        }
        $edidEntries | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $dumpFolder "DISPLAY-EDID.JSON") -Encoding utf8
    }
    catch {}

    # 5. Readme Summary
    $readme = @"
# Windows GPU UNLOCK - Diagnostic Dump Report
Generated: $timestamp

This diagnostic dump package contains complete system, GPU, display, and driver metadata
for compatibility analysis and bug reports:
- WINDOWS-INVENTORY.JSON: Windows hardware and PCI identifiers
- NVIDIA-SMI-DUMP.XML: Full GPU clock, power, memory, and VBIOS parameters
- SYSTEM-SUMMARY.JSON: Motherboard, BIOS, OS, Secure Boot, and BitLocker status
- DISPLAY-EDID.JSON: Active display monitors and EDID registry records
"@
    Set-Content -LiteralPath (Join-Path $dumpFolder "README.TXT") -Value $readme -Encoding utf8

    return $dumpFolder
}


function Export-EfiDiagnosticCaptures([string]$TargetDir, [string]$DeviceId = $null) {
    if (-not $TargetDir) { $TargetDir = Join-Path $Script:RepoRoot "diagnostic-captures" }
    if (-not (Test-Path -LiteralPath $TargetDir)) {
        New-Item -ItemType Directory -Path $TargetDir -Force | Out-Null
    }
    $info = Get-GpuHardwareInfo
    $summary = $info | ConvertTo-Json -Depth 4
    Set-Content -LiteralPath (Join-Path $TargetDir "SYSTEM-SUMMARY.JSON") -Value $summary -Encoding utf8
    return "Diagnostic captures exported to $TargetDir. (Full pre-boot logs are written to USB during UEFI boot in AD102-UNLOCK/)."
}


function Invoke-OfflineCompatibilityCheck {
    param(
        [Parameter(Mandatory)][string]$Folder,
        [string]$Language = 'en'
    )

    if (-not (Test-Path -LiteralPath $Folder)) {
        throw "Capture directory '$Folder' not found."
    }
    if (-not (Test-Path -LiteralPath $Script:CheckerPy)) {
        throw "Offline compatibility checker script not found at $Script:CheckerPy"
    }

    $raw = & python -I $Script:CheckerPy $Folder --lang $Language --json 2>&1
    $rawText = ($raw -join "`n").Trim()

    $jsonObj = $null
    try {
        $jsonObj = $rawText | ConvertFrom-Json
    }
    catch {
        return [pscustomobject]@{
            Status       = 'error'
            Badge        = '[ERROR]'
            BadgeColor   = 'Tomato'
            Summary      = "Failed to parse checker output into JSON."
            Scope        = "Unknown"
            Limitations  = "Python engine error or malformed diagnostic data."
            NextStep     = "Ensure Python 3.10+ is installed and verify capture file permissions."
            CaptureCount = 0
            Checks       = @()
            Observations = @()
            RawJson      = ""
            RawText      = $rawText
            Parsed       = $null
        }
    }

    $badge = switch ($jsonObj.status) {
        'candidate' { '[CANDIDATE] Eligible for Local Profile' }
        'needs_data' { '[NEEDS DATA] More Captures / Files Required' }
        'outside_scope' { '[OUTSIDE SCOPE] Hardware / Layout Outside Envelope' }
        'invalid_data' { '[INVALID DATA] Inconsistent / Corrupted Observations' }
        default { "[$($jsonObj.status.ToUpper())]" }
    }

    $badgeColor = switch ($jsonObj.status) {
        'candidate' { 'LightGreen' }
        'needs_data' { 'Gold' }
        'outside_scope' { 'DarkOrange' }
        'invalid_data' { 'Tomato' }
        default { 'Gray' }
    }

    return [pscustomobject]@{
        Status       = [string]$jsonObj.status
        Badge        = $badge
        BadgeColor   = $badgeColor
        Summary      = [string]$jsonObj.summary
        Scope        = [string]$jsonObj.scope
        Limitations  = [string]$jsonObj.limitations
        NextStep     = [string]$jsonObj.next_step
        CaptureCount = [int]$jsonObj.capture_count
        Checks       = @($jsonObj.checks)
        Observations = @($jsonObj.observations)
        RawJson      = $rawText
        RawText      = $rawText
        Parsed       = $jsonObj
    }
}


function Invoke-CompareCompatibilityReports {
    param(
        [Parameter(Mandatory)][string]$LeftReportPath,
        [Parameter(Mandatory)][string]$RightReportPath,
        [string]$Language = 'en'
    )

    if (-not (Test-Path -LiteralPath $LeftReportPath)) { throw "Reference report '$LeftReportPath' does not exist." }
    if (-not (Test-Path -LiteralPath $RightReportPath)) { throw "Target report '$RightReportPath' does not exist." }
    if (-not (Test-Path -LiteralPath $Script:ComparatorPy)) { throw "compare-compatibility.py not found at $Script:ComparatorPy" }

    $res = & python -I $Script:ComparatorPy $LeftReportPath $RightReportPath --lang $Language 2>&1
    return ($res -join "`r`n")
}


