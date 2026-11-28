@echo off
setlocal EnableDelayedExpansion
title Universal GPU Unlock - USB Updater
color 0F
cls

echo ====================================================================
echo      UNIVERSAL GPU UNLOCK - USB STICK UPDATER
echo ====================================================================
echo.

:: Usage: Update-USB.bat [F:] [--no-pause]
set "TARGET_DRIVE=%~1"
set "ASK_CONFIRM="

if not defined TARGET_DRIVE (
    set "ASK_CONFIRM=1"
    echo  Removable drives found:
    for /f "usebackq delims=" %%A in (`powershell -NoProfile -Command "Get-CimInstance Win32_LogicalDisk | Where-Object { $_.DriveType -eq 2 } | ForEach-Object { '{0}  {1}  {2}' -f $_.DeviceID, $_.FileSystem, $_.VolumeName }"`) do (
        echo    %%A
        if not defined TARGET_DRIVE set "TARGET_DRIVE=%%A"
    )
    echo.
)

if not defined TARGET_DRIVE (
    echo [!] No removable USB drive detected automatically.
    set /p "TARGET_DRIVE=Please enter your USB drive letter (e.g. F:): "
)

if not defined TARGET_DRIVE (
    echo [ERROR] No drive specified. Exiting.
    if not "%~2"=="--no-pause" pause
    exit /b 1
)

set "CLEAN_DRIVE=%TARGET_DRIVE:~0,1%:"

if defined ASK_CONFIRM (
    set "ANSWER="
    set /p "ANSWER=Update drive %CLEAN_DRIVE%? Type Y to continue, or another letter to pick that drive: "
    if /i "!ANSWER!"=="Y" (
        rem keep the detected drive
    ) else if defined ANSWER (
        set "CLEAN_DRIVE=!ANSWER:~0,1!:"
        set "ANSWER="
        set /p "ANSWER=Update drive !CLEAN_DRIVE!? [Y/N]: "
        if /i not "!ANSWER!"=="Y" goto :cancelled
    ) else (
        goto :cancelled
    )
)

set "DEPLOY_PS1=%~dp0Deploy-UsbCommon.ps1"
set "REPO_ROOT=%~dp0.."
if not exist "%DEPLOY_PS1%" (
    set "DEPLOY_PS1=%~dp0tools\Deploy-UsbCommon.ps1"
    set "REPO_ROOT=%~dp0"
)

echo.
echo  Target USB drive: %CLEAN_DRIVE%
echo  Deploying latest binaries, assets, and tools...
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& { try { . '%DEPLOY_PS1%'; Deploy-UniversalToUsb -Drive '%CLEAN_DRIVE%' -RepoRoot '%REPO_ROOT%' -OnLog { param($m, $c) if ($c) { Write-Host $m -ForegroundColor $c } else { Write-Host $m } }; Write-Host ''; Write-Host 'Pre-Flight Hardware Check (CSM & ReBAR):' -ForegroundColor Cyan; Show-GuCsmAndRebarPreFlight -OnLog { param($m, $c) if ($c) { Write-Host $m -ForegroundColor $c } else { Write-Host $m } } } catch { Write-Host ('[ERROR] ' + $_.Exception.Message) -ForegroundColor Red; exit 1 } }"
set "RC=%ERRORLEVEL%"

echo.
echo ====================================================================
if "%RC%"=="0" (
    echo   [OK] USB drive %CLEAN_DRIVE% updated.
) else (
    echo   [ERROR] Failed to update USB drive %CLEAN_DRIVE%.
)
echo ====================================================================

if not "%~2"=="--no-pause" pause
exit /b %RC%

:cancelled
echo Cancelled. Nothing was changed.
if not "%~2"=="--no-pause" pause
exit /b 1
