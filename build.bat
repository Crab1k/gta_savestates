@echo off
rem Build savestate.dll and GTASA-Savestates.exe (x86) with MSVC Build Tools.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /LD /EHsc /std:c++17 /W3 /DNDEBUG /Zi src\main.cpp src\arena.cpp src\hooks.cpp src\d3d.cpp src\state.cpp src\game.cpp src\lab.cpp src\census.cpp /Fobuild\ /Fdbuild\ /Fe:build\savestate.dll /link /DEBUG /MAP:build\savestate.map /OPT:REF /OPT:ICF user32.lib kernel32.lib gdi32.lib || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /DNDEBUG /DUNICODE /D_UNICODE src\launcher.cpp /Fobuild\ /Fe:build\GTASA-Savestates.exe /link /SUBSYSTEM:WINDOWS user32.lib kernel32.lib comdlg32.lib shell32.lib || exit /b 1
echo built build\savestate.dll and build\GTASA-Savestates.exe
