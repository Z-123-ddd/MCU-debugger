@echo off
rem ---------------------------------------------------------------------------
rem Stage a standalone folder: Qt runtime + data + SVD support packs.
rem   scripts\deploy.bat
rem Result: deploy\mcuRegDebugger.exe  (+ Qt DLLs, plugins, data\, svd\)
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

rem ---------------------------------------------------------------------------
rem Runtime data. The datasheets\ folder is 31 MB of vendor PDFs: they are the
rem SOURCE that scripts/build-package-tables.py turned into parts.json, and no
rem runtime code opens them. Shipping them would quadruple the package for
rem nothing, so they are excluded here.
rem ---------------------------------------------------------------------------
echo [INFO] copying data (excluding datasheets)
if exist "%ROOT%\data" (
    robocopy "%ROOT%\data" "%DEPLOY%\data" /E /NFL /NDL /NJH /NJS /NP /XD datasheets >nul
    if errorlevel 8 echo [WARN] robocopy data reported an error
)

rem ---------------------------------------------------------------------------
rem SVD support packs. The layout must stay exactly svd\manifest.json plus the
rem per-DFP directories: SvdLibrary resolves each manifest entry as
rem <root>\..\<svdPath>, i.e. deploy\svd\..\svd\N32H47x_DFP.1.2.0\N32H473.svd.
rem The .pack archives are the untouched vendor downloads; the app reads the
rem extracted .svd files, so they are left out.
rem ---------------------------------------------------------------------------
echo [INFO] copying svd support packs
if exist "%ROOT%\svd" (
    robocopy "%ROOT%\svd" "%DEPLOY%\svd" /E /NFL /NDL /NJH /NJS /NP /XF *.pack >nul
    if errorlevel 8 echo [WARN] robocopy svd reported an error
)

if exist "%ROOT%\README.md" copy /y "%ROOT%\README.md" "%DEPLOY%\" >nul
if exist "%ROOT%\LICENSE" copy /y "%ROOT%\LICENSE" "%DEPLOY%\" >nul

echo.
echo [OK] %DEPLOY%\mcuRegDebugger.exe
endlocal
