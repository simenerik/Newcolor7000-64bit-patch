@echo off
setlocal EnableDelayedExpansion
title Newcolor 7000 - 64-bit scanner patch

rem --- elevate if we are not already admin -------------------------------
net session >nul 2>&1
if errorlevel 1 (
    echo Requesting administrator rights...
    powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" >nul 2>&1
    exit /b
)

echo.
echo  Newcolor 7000 - 64-bit scanner patch
echo  ====================================
echo.

rem --- locate the installation -------------------------------------------
set "TARGET="
for %%D in ("C:\newcolor" "C:\Newcolor 7000 2.0" "C:\Program Files (x86)\Newcolor 7000 2.0" "C:\Program Files\Newcolor 7000 2.0") do (
    if exist "%%~D\NC7000.exe" set "TARGET=%%~D"
)

if not defined TARGET (
    echo  Could not find Newcolor automatically.
    echo.
    set /p "TARGET=  Full path to the folder containing NC7000.exe: "
)

if not exist "%TARGET%\NC7000.exe" (
    echo.
    echo  ERROR: NC7000.exe not found in "%TARGET%"
    echo  Nothing has been changed.
    goto :done
)

if not exist "%TARGET%\KSS32.dll" (
    echo.
    echo  ERROR: KSS32.dll not found. This looks like a demo-only install
    echo  without the scanner modules. The patch would have no effect.
    goto :done
)

echo  Found Newcolor in: %TARGET%
echo.

rem --- refuse to run while Newcolor is open ------------------------------
tasklist /fi "imagename eq NC7000.exe" 2>nul | find /i "NC7000.exe" >nul
if not errorlevel 1 (
    echo  ERROR: Newcolor is running. Close it and run this again.
    goto :done
)

rem --- back up the original, once ----------------------------------------
if exist "%TARGET%\HDSTI_stock.dll" (
    echo  Original already backed up as HDSTI_stock.dll - keeping it.
) else (
    if exist "%TARGET%\HDSTI.dll" (
        move "%TARGET%\HDSTI.dll" "%TARGET%\HDSTI_stock.dll" >nul
        if errorlevel 1 goto :failed
        echo  Backed up original to HDSTI_stock.dll
    )
)

rem --- install ------------------------------------------------------------
copy /y "%~dp0HDSTI.dll" "%TARGET%\HDSTI.dll" >nul
if errorlevel 1 goto :failed
echo  Installed HDSTI.dll

if exist "%TARGET%\hdsti.ini" (
    echo  hdsti.ini already present - keeping your settings.
) else (
    copy /y "%~dp0hdsti.ini" "%TARGET%\hdsti.ini" >nul
    echo  Installed hdsti.ini
)

powershell -NoProfile -Command "Unblock-File -Path '%TARGET%\HDSTI.dll'" >nul 2>&1

echo.
echo  Done.
echo.
echo  Next:
echo    1. Make sure the scanner is switched on BEFORE the PC boots.
echo    2. Run Newcolor AS ADMINISTRATOR - SCSI access requires it.
echo    3. Pick your scanner under Input source.
echo.
echo  Do NOT install the HDHLusd driver. It is 32-bit, Windows x64 will
echo  refuse it, and this patch removes the need for it entirely.
echo.
echo  If the scanner is not found, set Log=1 in hdsti.ini, retry, and
echo  read hdsti.log in the same folder.
echo.
goto :done

:failed
echo.
echo  ERROR: could not write to "%TARGET%".
echo  Close Newcolor and try again.

:done
echo.
pause
