@echo off
setlocal
cd /d "%~dp0"
title Universal GPU Unlock - Setup & Partition Manager

if /i "%~1"=="--new" goto :new_usb
if /i "%~1"=="--update" goto :update_usb
if /i "%~1"=="--partition" goto :create_part
if /i "%~1"=="--export-log" goto :export_log

:menu
cls
echo ====================================================================
echo      UNIVERSAL GPU UNLOCK - SETUP ^& DEPLOYMENT LAUNCHER
echo ====================================================================
echo.
echo   [1] Prepare New Bootable USB Drive (1-Click Setup + Pre-Flight)
echo   [2] Update Existing USB Boot Drive (Keep User Files)
echo   [3] Create Dedicated 512 MB Internal Unlock Partition (Hidden FAT32)
echo   [4] Open Dashboard Cockpit (Benchmarks, S3 Status ^& Power Limit)
echo   [5] Export Latest Boot Log (USB / Hidden GPU_UNLOCK / ESP)
echo   [Q] Quit
echo.
echo ====================================================================
set "CHOICE="
set /p "CHOICE=Select an option [1-5, Q]: "

if "%CHOICE%"=="1" goto :new_usb
if "%CHOICE%"=="2" goto :update_usb
if "%CHOICE%"=="3" goto :create_part
if "%CHOICE%"=="4" goto :dashboard
if "%CHOICE%"=="5" goto :export_log
if /i "%CHOICE%"=="Q" exit /b 0
goto :menu

:new_usb
set "SETUP_PS1=%~dp0tools\Setup-AdaUnlock.ps1"
if not exist "%SETUP_PS1%" set "SETUP_PS1=%~dp0Setup-AdaUnlock.ps1"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SETUP_PS1%"
if errorlevel 1 pause
exit /b %ERRORLEVEL%

:update_usb
set "UPD_BAT=%~dp0tools\Update-USB.bat"
if not exist "%UPD_BAT%" set "UPD_BAT=%~dp0Update-USB.bat"
call "%UPD_BAT%"
exit /b %ERRORLEVEL%

:create_part
set "PART_BAT=%~dp0tools\Create-Unlock-Partition.bat"
if exist "%PART_BAT%" (
    call "%PART_BAT%"
    exit /b %ERRORLEVEL%
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Create-Unlock-Partition.ps1"
exit /b %ERRORLEVEL%

:dashboard
call "%~dp0Launch-Dashboard.bat"
exit /b %ERRORLEVEL%

:export_log
set "EXP_PS1=%~dp0tools\Export-Boot-Log.ps1"
if not exist "%EXP_PS1%" set "EXP_PS1=%~dp0Export-Boot-Log.ps1"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%EXP_PS1%" -Destination "%~dp0Boot-Logs"
if errorlevel 1 pause
exit /b %ERRORLEVEL%
