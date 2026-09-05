@echo off
setlocal
title Newcolor 7000 - remove 64-bit scanner patch

net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" >nul 2>&1
    exit /b
)

echo.
echo  Removing the patch and restoring the original HDSTI.dll
echo.

set "TARGET="
for %%D in ("C:\newcolor" "C:\Newcolor 7000 2.0" "C:\Program Files (x86)\Newcolor 7000 2.0") do (
    if exist "%%~D\NC7000.exe" set "TARGET=%%~D"
)
if not defined TARGET set /p "TARGET=  Path to the Newcolor folder: "

if not exist "%TARGET%\HDSTI_stock.dll" (
    echo  No HDSTI_stock.dll found - the patch does not appear to be installed.
    goto :done
)

tasklist /fi "imagename eq NC7000.exe" 2>nul | find /i "NC7000.exe" >nul
if not errorlevel 1 (
    echo  ERROR: Newcolor is running. Close it and run this again.
    goto :done
)

del "%TARGET%\HDSTI.dll" >nul 2>&1
move "%TARGET%\HDSTI_stock.dll" "%TARGET%\HDSTI.dll" >nul
if errorlevel 1 (
    echo  ERROR: could not restore. Check permissions.
    goto :done
)
del "%TARGET%\hdsti.ini" >nul 2>&1
del "%TARGET%\hdsti.log" >nul 2>&1
echo  Original HDSTI.dll restored.
echo  Note: the scanner will no longer work on 64-bit Windows without the patch.

:done
echo.
pause
