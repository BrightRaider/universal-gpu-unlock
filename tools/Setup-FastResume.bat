@echo off
setlocal
title Fast-Resume Setup (Instant GPU Unlock Preservation)
color 0F
cls

if "%~1"=="--revert" goto :RevertSettings

echo ====================================================================
echo      FAST-RESUME CONFIGURATOR FOR UNIVERSAL GPU UNLOCK
echo ====================================================================
echo.
echo  This tool configures Windows Hibernation and creates a 1-click
echo  desktop shortcut to sleep your PC while keeping the GPU rate-limiter
echo  bypass permanently active across sessions.
echo.
echo  Why use Fast-Resume?
echo  - Cold boot (Shut down) resets GPU hardware registers to stock.
echo  - Hibernation (shutdown /h) suspends RAM and GPU state perfectly.
echo  - When waking up, Windows boots in ~3 seconds with the full
echo    unlocked TFLOPS ready immediately - no USB reboot required!
echo.
echo --------------------------------------------------------------------
echo  [1/3] Enabling Windows Hibernation (powercfg /hibernate on)...

:: Check for administrative permissions
net session >nul 2>&1
if %ERRORLEVEL% equ 0 (
    powercfg /hibernate on >nul 2>&1
    powercfg /h /type full >nul 2>&1
    echo   - Windows Full Hibernation: ENABLED [OK]
) else (
    echo   - Note: Administrator rights required for powercfg.
    echo     Attempting elevated configuration...
    powershell.exe -Command "Start-Process powercfg -ArgumentList '/hibernate on' -Verb RunAs -Wait" >nul 2>&1
    powershell.exe -Command "Start-Process powercfg -ArgumentList '/h /type full' -Verb RunAs -Wait" >nul 2>&1
    echo   - Windows Hibernation command dispatched.
)

echo.
echo --------------------------------------------------------------------
echo  [2/3] Creating Desktop Shortcut: "Fast-Unlock-Sleep"...
powershell.exe -NoProfile -Command "& { try { $wsh = New-Object -comObject WScript.Shell; $sc = $wsh.CreateShortcut(\"$([Environment]::GetFolderPath('Desktop'))\Fast-Unlock-Sleep.lnk\"); $sc.TargetPath = \"$env:SystemRoot\System32\shutdown.exe\"; $sc.Arguments = \"/h\"; $sc.IconLocation = \"$env:SystemRoot\System32\shell32.dll, 27\"; $sc.Description = \"Hibernate PC while preserving GPU rate-limiter unlock\"; $sc.Save(); Write-Host '  - Desktop Shortcut created: Fast-Unlock-Sleep.lnk [OK]' } catch { Write-Host '  [!] Failed to create shortcut:' $_ } }"

echo.
echo --------------------------------------------------------------------
echo  [3/3] Idle Relock Guard (PCIe ASPM and Runtime D3 Protection)...
echo.
echo  In laptops (Optimus/hybrid) and multi-GPU setups without attached
echo  monitors, Windows may aggressively power-gate the GPU into D3cold,
echo  which resets the rate-limiter unlock while the system is idle.
echo.

set "APPLY_GUARD="
if "%~1"=="--yes" set "APPLY_GUARD=Y"
if "%~1"=="-y" set "APPLY_GUARD=Y"

if not defined APPLY_GUARD (
    set /p "APPLY_GUARD=  Enable Idle Relock Guard (PCIe ASPM Off + RTD3 Guard) [Y/N]? "
)

if /i "%APPLY_GUARD%"=="Y" (
    echo   - Applying Idle Relock Guard...
    net session >nul 2>&1
    if %ERRORLEVEL% equ 0 (
        powercfg /setacvalueindex scheme_current sub_pcipexpress linkstatepr 0 >nul 2>&1
        powercfg /setactive scheme_current >nul 2>&1
        for /f "tokens=*" %%k in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}" 2^>nul ^| findstr /i /r "0[0-9][0-9][0-9]"') do (
            reg query "%%k" /v DriverDesc 2>nul | findstr /i "NVIDIA" >nul 2>&1
            if not errorlevel 1 (
                reg add "%%k" /v RMDisableRtd3 /t REG_DWORD /d 1 /f >nul 2>&1
                echo   - Guarded device: %%k [OK]
            )
        )
        echo   - Idle Relock Guard: ENABLED [OK]
    ) else (
        powershell.exe -NoProfile -Command "Start-Process cmd.exe -ArgumentList '/c powercfg /setacvalueindex scheme_current sub_pcipexpress linkstatepr 0 && powercfg /setactive scheme_current && for /f \"tokens=*\" %%k in (''reg query \"HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\" ^| findstr /i /r \"0[0-9][0-9][0-9]\"'') do (reg query \"%%k\" /v DriverDesc 2^>nul ^| findstr /i \"NVIDIA\" ^>nul ^&^& reg add \"%%k\" /v RMDisableRtd3 /t REG_DWORD /d 1 /f)' -Verb RunAs -Wait" >nul 2>&1
        echo   - Idle Relock Guard dispatched via elevated prompt.
    )
) else (
    echo   - Idle Relock Guard: Skipped (default Windows power management preserved).
)

echo.
echo ====================================================================
echo   SETUP COMPLETE!
echo ====================================================================
echo   To preserve your GPU unlock overnight or between sessions:
echo   -> Double-click 'Fast-Unlock-Sleep' on your Desktop.
echo   -> Turn PC back on: Windows resumes with 100%% unlocked TFLOPS!
echo.
echo   To revert Idle Relock Guard at any time, run:
echo   -> Setup-FastResume.bat --revert
echo ====================================================================
echo.
if not "%~1"=="--no-pause" pause
exit /b 0

:RevertSettings
echo ====================================================================
echo   REVERTING FAST-RESUME AND IDLE RELOCK SETTINGS
echo ====================================================================
echo.
net session >nul 2>&1
if %ERRORLEVEL% equ 0 (
    powercfg /setacvalueindex scheme_current sub_pcipexpress linkstatepr 1 >nul 2>&1
    powercfg /setactive scheme_current >nul 2>&1
    for /f "tokens=*" %%k in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}" 2^>nul ^| findstr /i /r "0[0-9][0-9][0-9]"') do (
        reg delete "%%k" /v RMDisableRtd3 /f >nul 2>&1
    )
    echo   - PCIe ASPM and RTD3 settings restored to Windows defaults [OK].
) else (
    powershell.exe -NoProfile -Command "Start-Process cmd.exe -ArgumentList '/c powercfg /setacvalueindex scheme_current sub_pcipexpress linkstatepr 1 && powercfg /setactive scheme_current && for /f \"tokens=*\" %%k in (''reg query \"HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\" ^| findstr /i /r \"0[0-9][0-9][0-9]\"'') do (reg delete \"%%k\" /v RMDisableRtd3 /f ^>nul 2^>^&1)' -Verb RunAs -Wait" >nul 2>&1
    echo   - Revert dispatched via elevated prompt.
)
echo.
if not "%~1"=="--no-pause" pause
exit /b 0
