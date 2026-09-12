@echo off
set PATH=C:\msys64\mingw32\bin;C:\msys64\usr\bin;%PATH%

:: ── Clean build option: "build.bat clean" ──
if /I "%1"=="clean" (
    echo [CLEAN] Removing build directory...
    if exist build rmdir /S /Q build
)

:: ── CMake configure + build ──
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build > build.log 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [BUILD FAILED] See build.log for details.
    exit /b 1
)
echo [BUILD OK] Deploying to test environment...

:: ── Deploy to both test instances ──
if exist "_TEST_MBAACC\MBAACC_1\cccaster\" (
    copy /Y "build\bin\CCCaster_v10.exe"      "_TEST_MBAACC\MBAACC_1\cccaster\"
    copy /Y "build\bin\libcccaster_hook.dll"   "_TEST_MBAACC\MBAACC_1\cccaster\"
)
if exist "_TEST_MBAACC\MBAACC_2\cccaster\" (
    copy /Y "build\bin\CCCaster_v10.exe"      "_TEST_MBAACC\MBAACC_2\cccaster\"
    copy /Y "build\bin\libcccaster_hook.dll"   "_TEST_MBAACC\MBAACC_2\cccaster\"
)
echo [DEPLOY OK] _TEST_MBAACC\MBAACC_1 + MBAACC_2
