@echo off
setlocal
:: Launch Control Center completely silently without keeping a console window open
start "" wscript.exe "%~dp0Universal-Control-Center.vbs"
exit
