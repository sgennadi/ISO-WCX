@echo off
setlocal
echo.
echo ISO-WCX Association Doctor - READ-ONLY inspection
echo Detects obsolete ISO\iso.wcx and the installed ISO-WCX plugin.
echo No Total Commander configuration will be changed.
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0ISO-WCX-Repair.ps1" %*
set "RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %RESULT%
