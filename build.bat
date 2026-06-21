@echo off
REM Build fucktheace.exe with MinGW-w64
REM Prerequisites: winget install BrechtSanders.WinLibs.POSIX.UCRT
REM Or adjust GCC_BIN below to your MinGW installation path.

setlocal

REM Auto-detect WinLibs from winget
set "GCC_BIN="
for /d %%d in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_*") do (
    if exist "%%d\mingw64\bin\gcc.exe" set "GCC_BIN=%%d\mingw64\bin"
)

if "%GCC_BIN%"=="" (
    echo [ERROR] MinGW-w64 not found. Install with:
    echo   winget install BrechtSanders.WinLibs.POSIX.UCRT
    exit /b 1
)

echo [INFO] Using: %GCC_BIN%

echo [1/2] Compiling resources...
"%GCC_BIN%\windres.exe" resource.rc -o resource.o
if errorlevel 1 exit /b 1

echo [2/2] Compiling executable...
"%GCC_BIN%\gcc.exe" -Os -s -mwindows -municode -static -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--stack,65536 fucktheace.c resource.o -o FuckTheACE.exe
if errorlevel 1 exit /b 1

echo.
echo === BUILD SUCCESS ===
for %%f in (FuckTheACE.exe) do echo   Size: %%~zf bytes
echo.
echo Run as Administrator: right-click FuckTheACE.exe -^> Run as administrator
