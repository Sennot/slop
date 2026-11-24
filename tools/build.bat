@echo off
rem Builds tools\slop_tool.exe with the local MSVC (no Geode needed).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d %~dp0
cl /nologo /std:c++20 /EHsc /O2 slop_tool.cpp ..\src\core.cpp /Fe:slop_tool.exe /Fo:%TEMP%\ >"%TEMP%\slop_tool_build.log" || type "%TEMP%\slop_tool_build.log"
