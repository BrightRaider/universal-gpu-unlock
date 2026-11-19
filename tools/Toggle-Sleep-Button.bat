@echo off
setlocal EnableDelayedExpansion
title Universal GPU Unlock - Start Menu Sleep Button Toggle
color 0F

:: Check Administrator Privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ====================================================================
    echo   Requesting Administrator privileges...
    echo ====================================================================
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

:menu
cls
echo ====================================================================
echo   UNIVERSAL GPU UNLOCK - Start Menu Sleep Button Toggle
echo ====================================================================
echo   Prevents accidental S3 Sleep while using volatile GPU unlock.
echo   Does NOT modify power plans, timeouts, or system background tasks.
echo --------------------------------------------------------------------

reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\FlyoutMenuSettings" /v ShowSleepOption 2>nul | findstr /i "0x0" >nul
if %errorlevel% equ 0 (
    echo   Current Status: [ HIDDEN / DISABLED ] (Protected from accidental sleep)
) else (
    echo   Current Status: [ VISIBLE / ENABLED ] (Default Windows)
)

echo --------------------------------------------------------------------
echo   [1] HIDE Sleep Button    (Disable "Sleep" in Start/Power Menu)
echo   [2] RESTORE Sleep Button (Show "Sleep" in Start/Power Menu)
echo   [3] Exit
echo ====================================================================
set "CHOICE="
set /p "CHOICE=Select an option [1-3]: "

if "%CHOICE%"=="1" goto :hide
if "%CHOICE%"=="2" goto :restore
if "%CHOICE%"=="3" exit /b 0
goto :menu

:hide
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\FlyoutMenuSettings" /v ShowSleepOption /t REG_DWORD /d 0 /f >nul
echo.
echo [OK] Sleep button has been HIDDEN from Start and Power menus.
goto :restart_explorer

:restore
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\FlyoutMenuSettings" /v ShowSleepOption /f >nul 2>&1
echo.
echo [OK] Sleep button has been RESTORED in Start and Power menus.
goto :restart_explorer

:restart_explorer
echo.
set "RESTART_EXP="
set /p "RESTART_EXP=Restart Windows Explorer now to apply immediately? [Y/N] (Default: Y): "
if /i "%RESTART_EXP%"=="n" goto :done
taskkill /f /im explorer.exe >nul 2>&1
timeout /t 1 /nobreak >nul
start explorer.exe
echo Explorer restarted successfully.

:done
echo.
echo Done. Press any key to return to menu...
pause >nul
goto :menu
