@echo off
rem Build the GTA III / Vice City savestates: build\savestate3.dll, GTA3-Savestates.exe, GTAVC-Savestates.exe (x86).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build\obj3 mkdir build\obj3
cl /nologo /O2 /MT /LD /EHsc /std:c++17 /W3 /DNDEBUG /Zi src3\main3.cpp src3\hooks3.cpp src3\d3d8.cpp src3\state3.cpp src3\game3.cpp src3\lab3.cpp src3\census3.cpp src\arena.cpp /Fobuild\obj3\ /Fdbuild\obj3\ /Fe:build\savestate3.dll /link /DEBUG /MAP:build\savestate3.map /OPT:REF /OPT:ICF user32.lib kernel32.lib gdi32.lib || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /DNDEBUG /DUNICODE /D_UNICODE "/DLAUNCHER_TITLE=L\"GTA III Savestates\"" "/DLAUNCHER_EXE=L\"gta3.exe\"" src\launcher.cpp /Fobuild\obj3\launcher3.obj /Fe:build\GTA3-Savestates.exe /link /SUBSYSTEM:WINDOWS user32.lib kernel32.lib comdlg32.lib shell32.lib || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /DNDEBUG /DUNICODE /D_UNICODE "/DLAUNCHER_TITLE=L\"GTA Vice City Savestates\"" "/DLAUNCHER_EXE=L\"gta-vc.exe\"" src\launcher.cpp /Fobuild\obj3\launchervc.obj /Fe:build\GTAVC-Savestates.exe /link /SUBSYSTEM:WINDOWS user32.lib kernel32.lib comdlg32.lib shell32.lib || exit /b 1
echo built build\savestate3.dll, build\GTA3-Savestates.exe, build\GTAVC-Savestates.exe
