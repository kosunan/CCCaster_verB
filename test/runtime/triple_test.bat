@echo off
setlocal
chcp 65001 >nul
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0triple_test.ps1" %*
set "TASK_EXIT=%ERRORLEVEL%"
if /I not "%~1"=="--check" pause
exit /b %TASK_EXIT%
