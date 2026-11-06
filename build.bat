@echo off
rem fq with MSVC.
rem   build.bat         build\fq.exe
rem   build.bat test    unit tests, then the end-to-end tests
setlocal
where cl >nul 2>nul || call :vcvars
where cl >nul 2>nul || (echo build.bat: cannot find the MSVC compiler & exit /b 1)
if not exist build mkdir build

set CFLAGS=/nologo /utf-8 /std:c11 /O2 /W4 /MT /D_CRT_SECURE_NO_WARNINGS
cl %CFLAGS% /Fobuild\ /Febuild\fq.exe main.c walk.c match.c regex.c out.c || exit /b 1

if /i "%1"=="test" (
    cl %CFLAGS% /I. /Fobuild\ /Febuild\unit.exe test\unit.c match.c regex.c || exit /b 1
    build\unit.exe || exit /b 1
    powershell -NoProfile -ExecutionPolicy Bypass -File test\run.ps1 build\fq.exe || exit /b 1
)
exit /b 0

:vcvars
rem through a file: "Program Files (x86)" inside a for /f breaks cmd's parser
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 0
"%VSWHERE%" -latest -property installationPath > "%TEMP%\fq-vsdir.txt"
set /p VSDIR=<"%TEMP%\fq-vsdir.txt"
del "%TEMP%\fq-vsdir.txt"
if defined VSDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
exit /b 0
