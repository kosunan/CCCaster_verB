@echo off
setlocal
cd /d "%~dp0"
set "PATH=C:\msys64\mingw32\bin;C:\msys64\usr\bin;%PATH%"
cmake -S src -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=C:/msys64/mingw32/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/msys64/mingw32/bin/g++.exe
if errorlevel 1 exit /b 1
cmake --build build -j8
if errorlevel 1 exit /b 1
ctest --test-dir build --output-on-failure --no-tests=error
exit /b %ERRORLEVEL%
