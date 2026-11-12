@echo off
rem Runs the given command inside a VS 2026 x64 developer environment (with Ninja on PATH).
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto run
rem A stray quote in PATH breaks vcvars; an inherited x86 environment would mix in x86 libraries.
set "PATH=%PATH:"=%"
set "__VSCMD_PREINIT_PATH="
set "VSCMD_VER="
set "INCLUDE="
set "LIB="
set "LIBPATH="
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
:run
%*
