@echo off
rem ---------------------------------------------------------------
rem  Argus - build script.  Usage:  build.cmd [debug|clean]
rem ---------------------------------------------------------------
setlocal enabledelayedexpansion
cd /d "%~dp0"

if /i "%~1"=="clean" (
    if exist build rmdir /s /q build
    echo Cleaned.
    exit /b 0
)

rem --- locate the MSVC environment ---------------------------------
set "VCVARS="
for %%E in (
    "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) do if not defined VCVARS if exist %%E set "VCVARS=%%~E"

if not defined VCVARS (
    echo ERROR: could not find vcvars64.bat. Install "Desktop development with C++".
    exit /b 1
)

if not defined ARGUS_ENV_READY (
    call "%VCVARS%" >nul || exit /b 1
    set ARGUS_ENV_READY=1
)

if not exist build mkdir build

rem --- flags --------------------------------------------------------
set "CFLAGS=/nologo /utf-8 /std:c++20 /W4 /WX /permissive- /EHsc /GR- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fobuild\ /Fdbuild\"
set "LFLAGS=/link /SUBSYSTEM:WINDOWS /MANIFEST:NO user32.lib gdi32.lib msimg32.lib gdiplus.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib windowscodecs.lib comdlg32.lib dwmapi.lib advapi32.lib"

if /i "%~1"=="debug" (
    set "CFLAGS=%CFLAGS% /Od /Zi /MTd /DDEBUG"
    set "LFLAGS=%LFLAGS% /DEBUG /INCREMENTAL:NO"
    set "OUT=build\argus-debug.exe"
) else (
    set "CFLAGS=%CFLAGS% /O2 /Oi /Ob2 /GS- /Gy /GL /MT /DNDEBUG"
    set "LFLAGS=%LFLAGS% /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO"
    set "OUT=build\argus.exe"
)

rem --- resources ----------------------------------------------------
rc /nologo /fobuild\app.res res\app.rc || exit /b 1

rem --- compile + link ----------------------------------------------
cl %CFLAGS% src\*.cpp build\app.res /Fe%OUT% %LFLAGS% || exit /b 1

echo.
for %%F in (%OUT%) do echo   Built %%F  (%%~zF bytes)
exit /b 0
