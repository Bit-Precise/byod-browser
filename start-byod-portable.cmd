@echo off
setlocal

rem Run the freshly built Chromium directly; no installer is required.
rem chrome.exe loads the sibling chrome.dll from this out\Grips directory.
set "CHROME=C:\src\chromium\src\out\Grips\chrome.exe"
set "PROFILE=C:\src\chromium\src\out\Grips\byod-portable-profile-20260920"

if not exist "%CHROME%" (
  echo Grips Chromium was not found:
  echo %CHROME%
  pause
  exit /b 1
)

if not exist "%PROFILE%" mkdir "%PROFILE%"

echo Starting portable BYOD Chromium (no installer)...
echo Executable: %CHROME%
echo Profile:    %PROFILE%
echo URL:        grips://exam.cs.ac.cn/

start "BYOD Portable" "%CHROME%" ^
  --user-data-dir="%PROFILE%" ^
  --no-first-run ^
  --no-default-browser-check ^
  "grips://exam.cs.ac.cn/"

endlocal
