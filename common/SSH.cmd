@echo off
rem SPDX-License-Identifier: GPL-2.0-only
call "%~dp0Run.cmd" --ssh %*
exit /b %errorlevel%
