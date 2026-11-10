@echo off
rem ---------------------------------------------------------------------------
rem Build MCU Register Debugger with the local Qt 6.8.3 / MinGW toolchain.
rem   scripts\build.bat            -> configure (first run) + incremental build
rem   scripts\build.bat --clean    -> wipe build dir and rebuild from scratch
rem ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

set "QT=D:\Qt\6.8.3\mingw_64"
set "MINGW=D:\Qt\Tools\mingw1310_64"
set "CMAKEDIR=D:\Qt\Tools\CMake_64\bin"
set "NINJA=D:\Qt\Tools\Ninja\ninja.exe"
set "BUILD=%ROOT%\build"

set "PATH=%QT%\bin;%MINGW%\bin;D:\Qt\Tools\Ninja;%CMAKEDIR%;%PATH%"

if not exist "%CMAKEDIR%\cmake.exe" (
    echo [ERROR] cmake not found at "%CMAKEDIR%\cmake.exe"
    exit /b 1
)
if not exist "%QT%\lib\cmake\Qt6\Qt6Config.cmake" (
    echo [ERROR] Qt6 not found at "%QT%"
    exit /b 1
)
if not exist "%NINJA%" (
    echo [ERROR] ninja not found at "%NINJA%"
    exit /b 1
)

set "CLEAN=0"
for %%A in (%*) do (
    if /I "%%A"=="--clean" set "CLEAN=1"
)

if "%CLEAN%"=="1" (
    echo [INFO] removing "%BUILD%"
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
)

if not exist "%BUILD%\CMakeCache.txt" (
    echo [INFO] configuring...
    "%CMAKEDIR%\cmake.exe" -S "%ROOT%" -B "%BUILD%" -G Ninja ^
        -DCMAKE_PREFIX_PATH="%QT%" ^
        -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
        -DCMAKE_BUILD_TYPE=Release
    if errorlevel 1 exit /b 1
)

echo [INFO] building...
"%CMAKEDIR%\cmake.exe" --build "%BUILD%" --parallel
if errorlevel 1 exit /b 1

echo.
echo [OK] %BUILD%\bin\mcuRegDebugger.exe
endlocal
