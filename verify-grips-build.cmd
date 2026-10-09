@echo off
setlocal EnableExtensions

rem Verify that all runtime files came from one successful Grips build.
rem This script must be run by Windows cmd.exe.

set "ROOT=C:\src\chromium\src"
set "OUT=%ROOT%\out\Grips"
if defined GRIPS_ROOT set "ROOT=%GRIPS_ROOT%"
if defined GRIPS_OUT set "OUT=%GRIPS_OUT%"

set "TOOL=%~dp0grips_build_manifest.py"
if not exist "%TOOL%" if exist "C:\Users\mryao\Downloads\grips_build_manifest.py" set "TOOL=C:\Users\mryao\Downloads\grips_build_manifest.py"
if not exist "%TOOL%" (
  echo ERROR: manifest verifier is missing: "%TOOL%"
  exit /b 2
)

cd /d "%ROOT%" || exit /b 2

rem Chromium checkouts commonly provide python3.exe; do not use `where` here
rem because the Windows App Execution Alias can make `where python3` report a
rem stale helper path even though invoking python3 directly works.
set "PYTHON=python3"
call %PYTHON% "%TOOL%" verify --root "%ROOT%" --out "%OUT%"
if errorlevel 1 exit /b %errorlevel%
exit /b %errorlevel%
