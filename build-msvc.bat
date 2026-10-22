@echo off
rem Build 1-Remind!.exe with Visual Studio (run from a
rem "x64 Native Tools Command Prompt for VS").
setlocal
cd /d "%~dp0"
if not exist dist mkdir dist

rc /nologo /I src /fo src\resource.res src\resource.rc
if errorlevel 1 exit /b 1

rem /O1 favour size, /MT static CRT => no VCRUNTIME dll needed,
rem /GL + /LTCG + /OPT:REF keep the exe tiny and fast to load.
cl /nologo /std:c++17 /O1 /Os /GL /MT /EHsc ^
   /DUNICODE /D_UNICODE ^
   src\main.cpp src\resource.res ^
   /Fe:"dist\1-Remind!.exe" ^
   /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF
if errorlevel 1 exit /b 1

echo Built dist\1-Remind!.exe
