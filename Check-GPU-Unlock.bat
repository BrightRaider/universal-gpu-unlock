@echo off
setlocal
title Universal GPU Unlock Status Check
color 0F
cls

set "PYTHON_EXE="

:: 1. Check known unsloth studio environment
if exist "C:\Users\%USERNAME%\.unsloth\studio\unsloth_studio\Scripts\python.exe" (
    set "PYTHON_EXE=C:\Users\%USERNAME%\.unsloth\studio\unsloth_studio\Scripts\python.exe"
    goto :RunCheck
)

:: 2. Check active Conda environment
if defined CONDA_PREFIX (
    if exist "%CONDA_PREFIX%\python.exe" (
        set "PYTHON_EXE=%CONDA_PREFIX%\python.exe"
        goto :RunCheck
    )
)

:: 3. Check system PATH python
where python.exe >nul 2>&1
if %ERRORLEVEL% equ 0 (
    set "PYTHON_EXE=python.exe"
    goto :RunCheck
)

:: 4. Check Windows Python launcher py
where py.exe >nul 2>&1
if %ERRORLEVEL% equ 0 (
    set "PYTHON_EXE=py.exe"
    goto :RunCheck
)

echo ====================================================================
echo   [ERROR] Python was not found on this system.
echo   Please install Python (https://www.python.org) and PyTorch.
echo ====================================================================
if not "%~1"=="--no-pause" pause
exit /b 1

:RunCheck
set "QUICK_CHECK="
if exist "%~dp0tools\quick_check.py" set "QUICK_CHECK=%~dp0tools\quick_check.py"
if not defined QUICK_CHECK if exist "%~dp0quick_check.py" set "QUICK_CHECK=%~dp0quick_check.py"

if not defined QUICK_CHECK (
    echo ====================================================================
    echo   [ERROR] quick_check.py was not found.
    echo ====================================================================
    if not "%~1"=="--no-pause" pause
    exit /b 1
)

"%PYTHON_EXE%" "%QUICK_CHECK%"
set "BENCH_ERR=%ERRORLEVEL%"
if "%BENCH_ERR%"=="0" (
    color 0A >nul 2>&1
) else (
    color 0C >nul 2>&1
)
echo.
if not "%~1"=="--no-pause" pause
exit /b %BENCH_ERR%
