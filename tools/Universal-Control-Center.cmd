@echo off
setlocal
title Universal GPU Unlock - Control Center

set "PS_SCRIPT=%~dp0Universal-Control-Center.ps1"
if not exist "%PS_SCRIPT%" set "PS_SCRIPT=%~dp0tools\Universal-Control-Center.ps1"

:: Check if running with administrative rights
net session >nul 2>&1
if %errorlevel% == 0 (
    start "" powershell.exe -STA -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "%PS_SCRIPT%"
    exit /b
)

:: If not admin, attempt elevated launch; if declined, fall back to user mode
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell.exe -ArgumentList '-STA -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File \"\"%PS_SCRIPT%\"\"' -Verb RunAs -ErrorAction SilentlyContinue" >nul 2>&1
if %errorlevel% neq 0 (
    start "" powershell.exe -STA -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "%PS_SCRIPT%"
)
exit /b
