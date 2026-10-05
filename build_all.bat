@echo off
rem Compila Outlast Online para 32 y 64 bits y genera dist\OutlastOnline.zip
setlocal
cd /d "%~dp0"
cmake -S . -B build32 -G "Visual Studio 18 2026" -A Win32 || goto :err
cmake --build build32 --config Release --target dinput8 || goto :err
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DOUTLAST_DLL32="%~dp0build32\Release\dinput8.dll" || goto :err
cmake --build build --config Release || goto :err
cmake --build build --config Release --target package_zip || goto :err
echo.
echo Listo: dist\OutlastOnline.zip
exit /b 0
:err
echo ERROR en la compilacion
exit /b 1
