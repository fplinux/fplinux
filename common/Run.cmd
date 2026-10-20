@echo off
rem SPDX-License-Identifier: GPL-2.0-only
setlocal
"%~dp0runtime\python.exe" -B -I "%~dp0fplinux.py" run %*
set "fplinux_status=%errorlevel%"
if not "%~1"=="" exit /b %fplinux_status%
echo.
pause
exit /b %fplinux_status%
