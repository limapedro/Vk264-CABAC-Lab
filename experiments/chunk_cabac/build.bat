@echo off
setlocal
set VC="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
call %VC% >nul
cd /d "%~dp0"
if not exist results mkdir results
cl /nologo /O2 /std:c++17 /EHsc /W3 chunk_cabac.cpp /Fe:chunk_cabac.exe
if errorlevel 1 exit /b 1
echo Built chunk_cabac.exe
