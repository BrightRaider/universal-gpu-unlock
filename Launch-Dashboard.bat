@echo off
setlocal

set "PYTHON_EXE="

:: 1. Check known unsloth studio environment (prefer pythonw.exe to suppress console)
if exist "C:\Users\%USERNAME%\.unsloth\studio\unsloth_studio\Scripts\pythonw.exe" (
    set "PYTHON_EXE=C:\Users\%USERNAME%\.unsloth\studio\unsloth_studio\Scripts\pythonw.exe"
    goto :RunDashboard
)
if exist "C:\Users\%USERNAME%\.unsloth\studio\unsloth_studio\Scripts\python.exe" (
    set "PYTHON_EXE=C:\Users\%USERNAME%\.unsloth\studio\unsloth_studio\Scripts\python.exe"
    goto :RunDashboard
)

:: 2. Check active Conda environment
if defined CONDA_PREFIX (
    if exist "%CONDA_PREFIX%\pythonw.exe" (
        set "PYTHON_EXE=%CONDA_PREFIX%\pythonw.exe"
        goto :RunDashboard
    )
    if exist "%CONDA_PREFIX%\python.exe" (
        set "PYTHON_EXE=%CONDA_PREFIX%\python.exe"
        goto :RunDashboard
    )
)

:: 3. Check system PATH for pythonw (windowless)
where pythonw.exe >nul 2>&1
if %ERRORLEVEL% equ 0 (
    set "PYTHON_EXE=pythonw.exe"
    goto :RunDashboard
)

:: 4. Check Windows Python launcher pyw
where pyw.exe >nul 2>&1
if %ERRORLEVEL% equ 0 (
    set "PYTHON_EXE=pyw.exe"
    goto :RunDashboard
)

:: 5. Fallback to standard console python
where python.exe >nul 2>&1
if %ERRORLEVEL% equ 0 (
    set "PYTHON_EXE=python.exe"
    goto :RunDashboard
)

echo ====================================================================
echo   [ERROR] Python was not found on this system.
echo   Please install Python (https://www.python.org) and PyTorch.
echo ====================================================================
pause
exit /b 1

:RunDashboard
set "DASHBOARD_SCRIPT=%~dp0tools\gpu_dashboard.py"
if not exist "%DASHBOARD_SCRIPT%" (
    echo ====================================================================
    echo   [ERROR] tools\gpu_dashboard.py was not found!
    echo ====================================================================
    pause
    exit /b 1
)

:: Start windowless GUI and exit the batch launcher immediately
start "" "%PYTHON_EXE%" "%DASHBOARD_SCRIPT%"
exit /b 0
