@echo off
setlocal

rem Always use the freshly built Grips Chromium so an older installed copy
rem cannot mask the current grips:// URL handler during testing.
set "CHROME=C:\src\chromium\src\out\Grips\chrome.exe"
set "ROOT=C:\src\chromium\src"
set "OUT=%ROOT%\out\Grips"
set "VERIFY=%~dp0verify-grips-build.cmd"

if not exist "%VERIFY%" if exist "C:\Users\mryao\Downloads\verify-grips-build.cmd" set "VERIFY=C:\Users\mryao\Downloads\verify-grips-build.cmd"
if not exist "%VERIFY%" (
  echo Build verifier is missing. Copy verify-grips-build.cmd and grips_build_manifest.py
  echo beside this BAT before testing.
  pause
  exit /b 2
)
call "%VERIFY%"
if errorlevel 1 (
  echo Refusing to launch: the Grips output is missing or contains mixed artifacts.
  pause
  exit /b 3
)

if not exist "%CHROME%" (
  echo Fresh Grips Chromium build was not found:
  echo %CHROME%
  pause
  exit /b 1
)

rem Use a fresh profile for the dynamic HTTPS exam shell.  Reusing the old
rem exam-test-no-code profile can keep a stale WebUI tab/process alive and
rem make it look as if the retired native exam-code page is still present.
if not defined BYOD_PROFILE set "BYOD_PROFILE=%OUT%\byod-portable-profile-20260920-v2"
set "PROFILE=%BYOD_PROFILE%"
if not exist "%PROFILE%" mkdir "%PROFILE%"

echo Starting BYOD Browser...
echo Profile: %PROFILE%
echo Sign in with Connect; assigned exams will be shown automatically.

start "BYOD Exam" /D "C:\src\chromium\src\out\Grips" "%CHROME%" ^
  --user-data-dir="%PROFILE%" ^
  --no-first-run ^
  --no-default-browser-check ^
  "grips://exam.cs.ac.cn/"

endlocal
