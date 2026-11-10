@echo off
setlocal EnableDelayedExpansion
title Universal GPU Unlock - Turing GSP Firmware Mode Toggle
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
echo   UNIVERSAL GPU UNLOCK - Turing (RTX 20 Series) GSP Firmware Toggle
echo ====================================================================
echo   Configures official NVIDIA Windows driver (nvlddmkm.sys) to bind to
echo   unlocked GSP firmware instead of falling back to CPU-RM mode.
echo.
echo   * Ada (RTX 40) and Ampere (RTX 30) run GSP-RM mode automatically.
echo   * Turing (RTX 20 / TITAN RTX) requires EnableGpuFirmware = 1 in Windows
echo     to activate unlocked compute (>85-105 TFLOPS vs 45-55 TFLOPS stock).
echo --------------------------------------------------------------------

:: Detect GPU Name and GSP Status via PowerShell
for /f "usebackq delims=" %%A in (`powershell -NoProfile -Command "Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\0*' | Where-Object { $_.DriverDesc -like '*NVIDIA*' } | ForEach-Object { $_.DriverDesc } | Select-Object -First 1"`) do (
    set "DETECTED_GPU=%%A"
)

for /f "usebackq delims=" %%A in (`powershell -NoProfile -Command "$val = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\0*' | Where-Object { $_.DriverDesc -like '*NVIDIA*' } | Select-Object -First 1).EnableGpuFirmware; if ($val -eq 1) { '1' } else { '0' }"`) do (
    set "GSP_STATUS=%%A"
)

if defined DETECTED_GPU (
    echo   Detected GPU   : !DETECTED_GPU!
) else (
    echo   Detected GPU   : NVIDIA Graphics Adapter (Standard Display Subkey)
)

if "!GSP_STATUS!"=="1" (
    echo   GSP Mode Status: [ ENABLED ] (GSP-RM Firmware Active on Turing)
) else (
    echo   GSP Mode Status: [ DISABLED / DEFAULT ] (Standard Legacy CPU-RM Mode)
)

echo --------------------------------------------------------------------
echo   [1] ENABLE GSP Firmware Mode    (EnableGpuFirmware = 1 for Turing Unlock)
echo   [2] RESTORE Factory CPU-RM Mode (Revert to default NVIDIA Windows driver)
echo   [3] RUN Compute Benchmark       (Launch Check-GPU-Unlock.bat)
echo   [4] Exit
echo ====================================================================
set "CHOICE="
set /p "CHOICE=Select an option [1-4]: "

if "%CHOICE%"=="1" goto :enable_gsp
if "%CHOICE%"=="2" goto :disable_gsp
if "%CHOICE%"=="3" goto :run_benchmark
if "%CHOICE%"=="4" exit /b 0
goto :menu

:enable_gsp
echo.
echo Enabling GSP Firmware Mode for NVIDIA display drivers...
powershell -NoProfile -Command "Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\0*' | Where-Object { $_.DriverDesc -like '*NVIDIA*' } | ForEach-Object { Set-ItemProperty -Path $_.PSPath -Name 'EnableGpuFirmware' -Value 1 -Type DWord; Write-Host '  [OK] Set EnableGpuFirmware = 1 on ' $_.DriverDesc -ForegroundColor Green }"
echo.
echo ====================================================================
echo   SUCCESS! GSP Firmware Mode enabled.
echo   Next steps:
echo   1. Reboot your PC and boot via USB [U] (or via internal ESP [I]).
echo   2. Run Check-GPU-Unlock.bat to verify >85-105 TFLOPS throughput.
echo ====================================================================
echo.
pause
goto :menu

:disable_gsp
echo.
echo Restoring factory CPU-RM mode...
powershell -NoProfile -Command "Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\0*' | Where-Object { $_.DriverDesc -like '*NVIDIA*' } | ForEach-Object { Remove-ItemProperty -Path $_.PSPath -Name 'EnableGpuFirmware' -ErrorAction SilentlyContinue; Write-Host '  [OK] Removed EnableGpuFirmware on ' $_.DriverDesc -ForegroundColor Yellow }"
echo.
echo ====================================================================
echo   SUCCESS! Registry setting removed. Factory driver behavior restored.
echo ====================================================================
echo.
pause
goto :menu

:run_benchmark
cls
if exist "%~dp0Check-GPU-Unlock.bat" (
    call "%~dp0Check-GPU-Unlock.bat"
) else (
    echo Check-GPU-Unlock.bat not found in current directory.
    pause
)
goto :menu
