@echo off
setlocal
set "DEPOT_TOOLS_UPDATE=0"
cd /d C:\src\chromium || exit /b 2
call gclient runhooks
exit /b %errorlevel%
