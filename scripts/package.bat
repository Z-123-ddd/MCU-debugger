@echo off
rem ---------------------------------------------------------------------------
rem Build the Windows installer.
rem   scripts\package.bat
rem Steps: stage deploy\ (Qt runtime + data + SVD packs) -> makensis -> dist\
rem Result: dist\McuRegDebugger-<version>-setup.exe
rem NOTE: messages are ASCII only - cmd.exe uses the OEM codepage and would
rem       mangle UTF-8 Chinese text inside batch files.
rem ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

set "DEPLOY=%ROOT%\deploy"
set "DIST=%ROOT%\dist"
set "NSI=%ROOT%\scripts\installer.nsi"

rem ---- version, read from CMakeLists so it cannot drift -------------------
set "VERSION="
for /f "usebackq tokens=1,2" %%A in (`findstr /r /c:"^    VERSION [0-9]" "%ROOT%\CMakeLists.txt"`) do (
    if not defined VERSION set "VERSION=%%B"
)
if not defined VERSION (
    echo [ERROR] could not read the version from CMakeLists.txt
    exit /b 1
)
echo [INFO] version %VERSION%

rem ---- toolchain ---------------------------------------------------------
set "MAKENSIS="
if defined NSIS_HOME if exist "%NSIS_HOME%\makensis.exe" set "MAKENSIS=%NSIS_HOME%\makensis.exe"
if not defined MAKENSIS if exist "%ROOT%\tools\nsis\makensis.exe" set "MAKENSIS=%ROOT%\tools\nsis\makensis.exe"
if not defined MAKENSIS if exist "%ProgramFiles(x86)%\NSIS\makensis.exe" set "MAKENSIS=%ProgramFiles(x86)%\NSIS\makensis.exe"
if not defined MAKENSIS if exist "%ProgramFiles%\NSIS\makensis.exe" set "MAKENSIS=%ProgramFiles%\NSIS\makensis.exe"
if not defined MAKENSIS for %%I in (makensis.exe) do if not "%%~$PATH:I"=="" set "MAKENSIS=%%~$PATH:I"

if not defined MAKENSIS (
    echo [ERROR] makensis.exe not found.
    echo         fetch the portable toolchain once with:
    echo           powershell -ExecutionPolicy Bypass -File scripts\fetch-nsis.ps1
    echo         or point NSIS_HOME at an existing NSIS install.
    exit /b 1
)
echo [INFO] makensis: %MAKENSIS%

rem ---- stage the payload -------------------------------------------------
call "%ROOT%\scripts\deploy.bat"
if errorlevel 1 (
    echo [ERROR] deploy.bat failed
    exit /b 1
)

rem The payload must be complete: an installer that ships without the SVD packs
rem or the device database looks fine until the user connects a board.
if not exist "%DEPLOY%\mcuRegDebugger.exe" (
    echo [ERROR] deploy is missing mcuRegDebugger.exe
    exit /b 1
)
if not exist "%DEPLOY%\svd\manifest.json" (
    echo [ERROR] deploy is missing svd\manifest.json
    exit /b 1
)
if not exist "%DEPLOY%\data\parts.json" (
    echo [ERROR] deploy is missing data\parts.json
    exit /b 1
)

rem ---- J-Link runtime dependencies ---------------------------------------
rem The application cannot talk to hardware without SEGGER J-Link software.
rem Two separate payloads are involved:
rem   * the N32 device database (vendor-supplied, redistributable) - REQUIRED,
rem     because SEGGER installer knows nothing about Nationstech parts and
rem     J-Link answers "unknown device" without it;
rem   * SEGGER own installer - OPTIONAL. Commercial software, not committed;
rem     the packager downloads it once. Without it the installer still builds
rem     and tells the user to install J-Link themselves.
set "JLINK_DEVICES=%ROOT%\tools\jlink-devices"
set "JLINK_INSTALLER=%ROOT%\tools\jlink-installer"

if not exist "%JLINK_DEVICES%\v77\Nations-JLinkDevices.xml" (
    echo [ERROR] J-Link device database is missing:
    echo         %JLINK_DEVICES%\v77\Nations-JLinkDevices.xml
    echo         fetch it with: python scripts\fetch-jlink.py
    exit /b 1
)
if not exist "%JLINK_DEVICES%\v64\Nations-JLinkDevices.xml" (
    echo [ERROR] J-Link device database variant v64 is missing
    echo         fetch it with: python scripts\fetch-jlink.py
    exit /b 1
)
echo [INFO] N32 device database: present

set "JLINK_SETUP_COUNT=0"
if exist "%JLINK_INSTALLER%\*.exe" (
    for %%F in ("%JLINK_INSTALLER%\*.exe") do set /a JLINK_SETUP_COUNT+=1
)
if %JLINK_SETUP_COUNT%==0 (
    echo [WARN] no SEGGER J-Link installer staged in tools\jlink-installer
    echo        the resulting installer will ask the user to install J-Link manually
) else (
    echo [INFO] SEGGER J-Link installer bundled: %JLINK_SETUP_COUNT% file^(s^)
)

if not exist "%DIST%" mkdir "%DIST%"
set "OUT=%DIST%\McuRegDebugger-%VERSION%-setup.exe"
if exist "%OUT%" del /q "%OUT%"

echo [INFO] compiling installer
"%MAKENSIS%" /V2 /DSHORT="1" ^
    /DAPP_VERSION=%VERSION% ^
    /DDEPLOY_DIR="%DEPLOY%" ^
    /DOUT_FILE="%OUT%" ^
    /DJLINK_DEVICES_DIR="%JLINK_DEVICES%" ^
    /DJLINK_INSTALLER_DIR="%JLINK_INSTALLER%" ^
    "%NSI%"
if errorlevel 1 (
    echo [ERROR] makensis failed
    exit /b 1
)

for %%I in ("%OUT%") do set "SIZE=%%~zI"
set /a SIZE_MB=%SIZE% / 1048576

echo.
echo [OK] %OUT%  (%SIZE_MB% MB)
endlocal
