@echo off
setlocal
rem Run inside the interactive Windows session, with an isolated browser profile.
rem Expected layout: this script under build/, unpacked files under verification/.
set "BYOD_SMOKE_VERSION=%~1"
if not defined BYOD_SMOKE_VERSION set "BYOD_SMOKE_VERSION=152.0.7947.0"
call python3 -u "%~dp0verify-byod-user-agent.py" --chrome "%~dp0..\verification\runtime\chrome.exe" --version %BYOD_SMOKE_VERSION% --headless > "%~dp0..\interactive-smoke.log" 2>&1
set "BYOD_SMOKE_RESULT=%ERRORLEVEL%"
> "%~dp0..\interactive-smoke.exit" echo %BYOD_SMOKE_RESULT%
exit /b %BYOD_SMOKE_RESULT%
