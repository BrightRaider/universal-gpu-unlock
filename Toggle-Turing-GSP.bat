@echo off
setlocal
call "%~dp0tools\Toggle-Turing-GSP.bat" %*
exit /b %ERRORLEVEL%
