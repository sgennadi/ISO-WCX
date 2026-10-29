@echo off
setlocal
echo.
echo ISO-WCX - register all supported image extensions in Total Commander
echo ZIP, RAR, 7z, user buttons and other plugins are preserved.
echo Close ALL Total Commander windows before applying changes.
echo A byte-exact backup of each modified INI will be created.
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0ISO-WCX-AssociateAll.ps1" -Apply %*
set "RESULT=%ERRORLEVEL%"
echo.
if not "%RESULT%"=="0" echo No completed registration. Check the error above.
pause
exit /b %RESULT%
