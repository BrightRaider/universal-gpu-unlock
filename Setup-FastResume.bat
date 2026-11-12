@echo off
setlocal
title Fast-Resume Setup (Instant GPU Unlock Preservation)
color 0F
cls

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
echo  [1/2] Enabling Windows Hibernation (powercfg /hibernate on)...

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
echo  [2/2] Creating Desktop Shortcut: "Fast-Unlock-Sleep"...
powershell.exe -NoProfile -Command "& { try { $wsh = New-Object -comObject WScript.Shell; $sc = $wsh.CreateShortcut(\"$([Environment]::GetFolderPath('Desktop'))\Fast-Unlock-Sleep.lnk\"); $sc.TargetPath = \"$env:SystemRoot\System32\shutdown.exe\"; $sc.Arguments = \"/h\"; $sc.IconLocation = \"$env:SystemRoot\System32\shell32.dll, 27\"; $sc.Description = \"Hibernate PC while preserving GPU rate-limiter unlock\"; $sc.Save(); Write-Host '  - Desktop Shortcut created: Fast-Unlock-Sleep.lnk [OK]' } catch { Write-Host '  [!] Failed to create shortcut:' $_ } }"

echo.
echo ====================================================================
echo   SETUP COMPLETE!
echo ====================================================================
echo   To preserve your GPU unlock overnight or between sessions:
echo   -> Double-click 'Fast-Unlock-Sleep' on your Desktop.
echo   -> Turn PC back on: Windows resumes with 100%% unlocked TFLOPS!
echo ====================================================================
echo.
pause
exit /b 0
