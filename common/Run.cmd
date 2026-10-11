@echo off
rem SPDX-License-Identifier: GPL-2.0-only
setlocal
"%~dp0runtime\python.exe" -B -I "%~dp0Run.py" %*
set "fplinux_status=%errorlevel%"
echo.
pause
exit /b %fplinux_status%
