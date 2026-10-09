@echo off
setlocal
cd /d C:\src\chromium\src || exit /b 2
rem Run only after reviewing ninja -n chrome; never clean or change args.gn.
set "BYOD_MANIFEST_BACKUP=C:\src\chromium\byod-xhttp-tools-20261007\byod-build-manifest.pre-xhttp-%RANDOM%-%RANDOM%.json"
if exist out\Grips\byod-build-manifest.json (
  if exist "%BYOD_MANIFEST_BACKUP%" exit /b 3
  move out\Grips\byod-build-manifest.json "%BYOD_MANIFEST_BACKUP%" >nul
  if errorlevel 1 exit /b 4
)
third_party\ninja\ninja.exe -C out\Grips -j8 chrome
if errorlevel 1 exit /b 5
call python3 C:\src\chromium\byod-xhttp-tools-20261007\grips_build_manifest.py write --root C:\src\chromium\src --out C:\src\chromium\src\out\Grips
if errorlevel 1 exit /b 6
call python3 C:\src\chromium\byod-xhttp-tools-20261007\grips_build_manifest.py verify --root C:\src\chromium\src --out C:\src\chromium\src\out\Grips
exit /b %errorlevel%
