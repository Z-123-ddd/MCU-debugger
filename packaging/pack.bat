@echo off
rem ---------------------------------------------------------------------------
rem Build the NSIS installer from the staged deploy\ folder.
rem   1) scripts\build.bat
rem   2) scripts\deploy.bat
rem   3) packaging\pack.bat
rem Result: dist\McuRegDebugger-<version>-setup.exe
rem NOTE: ASCII messages only (cmd.exe OEM codepage).
rem ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "MAKENSIS=D:\NSIS\makensis.exe"

if not exist "%MAKENSIS%" (
    echo [ERROR] NSIS not found: %MAKENSIS%
    echo         install NSIS or edit MAKENSIS in this script
    exit /b 1
)

if not exist "%ROOT%\deploy\mcuRegDebugger.exe" (
    echo [ERROR] not found: %ROOT%\deploy\mcuRegDebugger.exe
    echo         run scripts\build.bat and scripts\deploy.bat first
    exit /b 1
)

if not exist "%ROOT%\dist" mkdir "%ROOT%\dist"

echo [INFO] makensis packaging\installer.nsi
"%MAKENSIS%" /V2 "%ROOT%\packaging\installer.nsi"
if errorlevel 1 (
    echo [ERROR] NSIS packaging failed
    exit /b 1
)

echo.
echo [OK] dist contents:
dir /b "%ROOT%\dist"
endlocal
