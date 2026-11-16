@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /nologo /O2 /EHsc /I..\third_party\OpenXR-SDK\include xrprobe.cpp ..\third_party\build-openxr\src\loader\Release\openxr_loader.lib /Fe:xrprobe.exe
