@echo off
setlocal
call "%~dp0tools\Toggle-Sleep-Button.bat" %*
exit /b %ERRORLEVEL%
