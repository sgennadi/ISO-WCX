@echo off
setlocal
echo.
echo ISO-WCX Association Repair
echo This will update ONLY a missing ISO\iso.wcx entry in wincmd.ini.
echo The original INI is backed up automatically.
echo Close all Total Commander windows BEFORE continuing.
echo.
set /p "CONFIRM=Type YES to repair the old ISO association: "
if /I not "%CONFIRM%"=="YES" (
    echo Cancelled: nothing changed.
    exit /b 0
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0ISO-WCX-Repair.ps1" -Apply %*
set "RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %RESULT%
