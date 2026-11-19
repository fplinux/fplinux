@echo off
rem SPDX-License-Identifier: GPL-2.0-only
setlocal
"%~dp0runtime\python.exe" -B -I "%~dp0fplinux.py" %*
set "fplinux_status=%errorlevel%"
if "%~1"=="" if not "%fplinux_status%"=="0" pause
exit /b %fplinux_status%
