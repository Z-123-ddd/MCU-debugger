@echo off
rem ---------------------------------------------------------------------------
rem Stage a standalone folder (Qt runtime + data + probe-rs when available).
rem   scripts\deploy.bat
rem Result: deploy\mcuRegDebugger.exe  (+ Qt DLLs, plugins, data\)
rem NOTE: messages are ASCII only - cmd.exe uses the OEM codepage and would
rem       mangle UTF-8 Chinese text inside batch files.
rem ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

set "QT=D:\Qt\6.8.3\mingw_64"
set "BUILD=%ROOT%\build"
set "EXE=%BUILD%\bin\mcuRegDebugger.exe"
set "DEPLOY=%ROOT%\deploy"

if not exist "%EXE%" (
    echo [ERROR] not found: %EXE%
    echo         run scripts\build.bat first
    exit /b 1
)

echo [INFO] cleaning %DEPLOY%
if exist "%DEPLOY%" rmdir /s /q "%DEPLOY%"
mkdir "%DEPLOY%"
if errorlevel 1 exit /b 1

echo [INFO] copying executable
copy /y "%EXE%" "%DEPLOY%\" >nul
if errorlevel 1 exit /b 1

echo [INFO] windeployqt
"%QT%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw --compiler-runtime "%DEPLOY%\mcuRegDebugger.exe"
if errorlevel 1 echo [WARN] windeployqt returned non-zero, continuing

rem windeployqt only ships qwindows.dll; add the offscreen plugin so headless
rem runs (--selftest / --render / CI) work from the packaged folder too.
if exist "%QT%\plugins\platforms\qoffscreen.dll" (
    echo [INFO] copying offscreen platform plugin
    copy /y "%QT%\plugins\platforms\qoffscreen.dll" "%DEPLOY%\platforms\" >nul
)

echo [INFO] copying data
if exist "%ROOT%\data" xcopy /e /i /y /q "%ROOT%\data" "%DEPLOY%\data" >nul

if exist "%ROOT%\tools\probe-rs\probe-rs.exe" (
    echo [INFO] bundling probe-rs
    xcopy /e /i /y /q "%ROOT%\tools\probe-rs" "%DEPLOY%\tools\probe-rs" >nul
) else (
    echo [WARN] tools\probe-rs\probe-rs.exe not found
    echo        install probe-rs separately, or run scripts\fetch-probe-rs.ps1 and re-deploy
)

if exist "%ROOT%\README.md" copy /y "%ROOT%\README.md" "%DEPLOY%\" >nul

echo.
echo [OK] %DEPLOY%\mcuRegDebugger.exe
endlocal
