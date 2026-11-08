@echo off
setlocal
title Universal GPU Unlock - Control Center

:: Check if running with administrative rights
net session >nul 2>&1
if %errorlevel% == 0 (
    start "" powershell.exe -STA -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "%~dp0tools\Universal-Control-Center.ps1"
    exit /b
)

:: If not admin, attempt elevated launch; if declined, fall back to user mode
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell.exe -ArgumentList '-STA -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File \"\"%~dp0tools\Universal-Control-Center.ps1\"\"' -Verb RunAs -ErrorAction SilentlyContinue" >nul 2>&1
if %errorlevel% neq 0 (
    start "" powershell.exe -STA -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "%~dp0tools\Universal-Control-Center.ps1"
)
exit /b
