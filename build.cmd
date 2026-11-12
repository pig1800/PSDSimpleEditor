@echo off
rem build.cmd [debug|release]   (default: release) -> build\x64-<config>\PSDSimpleEditor.exe
setlocal
set "CFG=%~1"
if "%CFG%"=="" set "CFG=release"
cd /d "%~dp0"
call scripts\vsenv.cmd cmake --preset x64-%CFG% || exit /b 1
call scripts\vsenv.cmd cmake --build --preset x64-%CFG% || exit /b 1
