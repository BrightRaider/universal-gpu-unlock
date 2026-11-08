@echo off
setlocal
title Universal GPU Unlock - Control Center Launcher

:: Check for administrative rights
net session >nul 2>&1
if %errorLevel% == 0 (
    goto :RunAdmin
) else (
    goto :PromptElevation
)

:PromptElevation
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"\"%~dp0Universal-Control-Center.ps1\"\"' -Verb RunAs"
exit /b

:RunAdmin
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Universal-Control-Center.ps1" %*
if %errorlevel% neq 0 (
    echo.
    echo Control Center exited with code %errorlevel%.
    pause
)
