@echo off
call "%~dp0..\build.bat" %*
exit /b %ERRORLEVEL%
