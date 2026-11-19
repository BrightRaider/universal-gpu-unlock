@echo off
setlocal EnableDelayedExpansion
title Universal GPU Unlock - 1-Click USB Updater
color 0F
cls

echo ====================================================================
echo      UNIVERSAL GPU UNLOCK - 1-CLICK USB STICK QUICK UPDATER
echo ====================================================================
echo.

set "TARGET_DRIVE=%~1"

if not defined TARGET_DRIVE (
    for /f "usebackq delims=" %%A in (`powershell -NoProfile -Command "Get-CimInstance Win32_LogicalDisk | Where-Object { $_.DriveType -eq 2 } | Select-Object -ExpandProperty DeviceID"`) do (
        if not defined TARGET_DRIVE set "TARGET_DRIVE=%%A"
    )
)

if not defined TARGET_DRIVE (
    echo [!] No removable USB drive detected automatically.
    set /p "TARGET_DRIVE=Please enter your USB Drive Letter (e.g. F:): "
)

if not defined TARGET_DRIVE (
    echo [ERROR] No drive specified. Exiting.
    pause
    exit /b 1
)

:: Ensure drive ends with colon
set "CLEAN_DRIVE=%TARGET_DRIVE:~0,1%:"

echo  Target USB Drive: %CLEAN_DRIVE%
echo  Deploying latest binaries, assets, and tools...
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& { . '%~dp0tools\Deploy-UsbCommon.ps1'; Deploy-UniversalToUsb -Drive '%CLEAN_DRIVE%' -RepoRoot '%~dp0' -OnLog { param($m, $c) if ($c) { Write-Host $m -ForegroundColor $c } else { Write-Host $m } } }"

if %ERRORLEVEL% equ 0 (
    echo.
    echo ====================================================================
    echo   [OK] USB DRIVE %CLEAN_DRIVE% UPDATED SUCCESSFULLY IN 2 SECONDS!
    echo ====================================================================
) else (
    echo.
    echo ====================================================================
    echo   [ERROR] Failed to update USB drive %CLEAN_DRIVE%.
    echo ====================================================================
)

if not "%~2"=="--no-pause" pause
exit /b %ERRORLEVEL%
