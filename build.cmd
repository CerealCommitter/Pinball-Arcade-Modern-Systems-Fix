@echo off
rem Builds bin\xinput1_3.dll (the Pinball Arcade fix) with the MSVC x86 toolset.
setlocal
cd /d "%~dp0"
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo MSVC tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
if not exist bin mkdir bin
if not exist obj mkdir obj
cl /nologo /O2 /MT /W3 /std:c++17 /EHsc /GS- /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Foobj\ ^
   src\PinballFix.cpp src\Display.cpp src\proxy_xinput.cpp ^
   /LD /Febin\xinput1_3.dll ^
   /link /DEF:src\xinput1_3.def /SAFESEH:NO user32.lib kernel32.lib gdi32.lib winmm.lib || exit /b 1
echo Built bin\xinput1_3.dll
