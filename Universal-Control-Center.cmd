@echo off
setlocal
call "%~dp0tools\Universal-Control-Center.cmd" %*
exit /b %ERRORLEVEL%
