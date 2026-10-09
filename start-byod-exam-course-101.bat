@echo off
setlocal
set "CHROME=C:\src\chromium\src\out\Grips\chrome.exe"
set "PROFILE=C:\src\chromium\src\out\Grips\byod-portable-profile-20260920-v2"
if not exist "%CHROME%" (
  echo Fresh Grips Chromium build was not found: %CHROME%
  pause
  exit /b 1
)
if not exist "%PROFILE%" mkdir "%PROFILE%"
if not exist "C:\src\chromium\src\out\Grips\chrome.dll" exit /b 1
if not exist "C:\src\chromium\src\out\Grips\resources.pak" exit /b 1
start "BYOD Exam" /D "C:\src\chromium\src\out\Grips" "%CHROME%" ^
  --user-data-dir="%PROFILE%" ^
  --no-first-run ^
  --no-default-browser-check ^
  "grips://exam.cs.ac.cn/"
endlocal
