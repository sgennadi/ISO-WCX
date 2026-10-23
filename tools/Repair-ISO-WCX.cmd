@echo off
setlocal
set "REPAIR=%~dp0Repair-ISO-WCX.ps1"
if not exist "%REPAIR%" (
    echo Repair-ISO-WCX.ps1 not found next to this BAT file.
    pause
    exit /b 2
)
echo.
echo ISO-WCX Total Commander association repair
echo Preview mode: no files will be changed yet.
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%REPAIR%"
if errorlevel 1 (
    echo.
    echo Preview failed. Run in PowerShell with -IniPath to select wincmd.ini.
    pause
    exit /b 1
)
echo.
echo Before applying, CLOSE ALL Total Commander windows.
echo The repair preserves a backup of wincmd.ini and does not touch other plugins.
choice /C YN /N /M "Apply the repair now? [Y/N] "
if errorlevel 2 exit /b 0
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%REPAIR%" -Apply
if errorlevel 1 (
    echo.
    echo Repair was NOT confirmed. Check the error above.
    pause
    exit /b 1
)
echo.
echo Reopen Total Commander and try Ctrl+PgDn on an ISO file.
pause
