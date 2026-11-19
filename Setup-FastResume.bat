@echo off
setlocal
call "%~dp0tools\Setup-FastResume.bat" %*
exit /b %ERRORLEVEL%
