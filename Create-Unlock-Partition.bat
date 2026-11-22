@echo off
setlocal EnableDelayedExpansion
title Universal GPU Unlock - Create Dedicated 512MB FAT32 Partition

:: Check Administrator Privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ====================================================================
    echo   Requesting Administrator privileges...
    echo ====================================================================
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Create-Unlock-Partition.ps1" %*
exit /b %ERRORLEVEL%
