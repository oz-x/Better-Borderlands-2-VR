@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /nologo /O2 /EHsc stackdump.cpp /Fe:stackdump.exe
