@echo off
rem SPDX-License-Identifier: GPL-2.0-only
setlocal
call "%~dp0Run.cmd" --loader-trace %*
set "fplinux_status=%errorlevel%"
if not "%~1"=="" exit /b %fplinux_status%
echo.
pause
exit /b %fplinux_status%
