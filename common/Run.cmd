@echo off
rem SPDX-License-Identifier: GPL-2.0-only
setlocal
for %%C in (run console verify prepare nand) do if "%~1"=="%%C" goto command
"%~dp0runtime\python.exe" -B -I "%~dp0fplinux.py" run %*
goto complete
:command
"%~dp0runtime\python.exe" -B -I "%~dp0fplinux.py" %*
:complete
set "fplinux_status=%errorlevel%"
if not "%~1"=="" exit /b %fplinux_status%
echo.
pause
exit /b %fplinux_status%
