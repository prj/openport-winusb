@echo off
rem Optional first argument: output DLL path (the default one is locked while a J2534 app has the shim loaded).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cd /d %~dp0
set OUT=%~1
if "%OUT%"=="" set OUT=op20winusb.dll
rc -nologo op20winusb.rc || exit /b 1
cl -nologo -W3 -O2 -MT -LD -Zi -D_CRT_SECURE_NO_WARNINGS op20winusb.c op20winusb.res -link -def:op20winusb.def -map -debug -pdbaltpath:%%_PDB%% -out:"%OUT%" winusb.lib user32.lib || exit /b 1
