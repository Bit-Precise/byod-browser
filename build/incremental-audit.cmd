@echo off
setlocal
cd /d C:\src\chromium\src || exit /b 2
rem Only after ninja -n chrome has been reviewed. Do not change args.gn.
set "BYOD_AUDIT_TOOLS=C:\src\chromium\byod-audit-tools-20261007"
if exist out\Grips\byod-build-manifest.json (
  move out\Grips\byod-build-manifest.json "%BYOD_AUDIT_TOOLS%\manifest-before-%RANDOM%-%RANDOM%.json" >nul
  if errorlevel 1 exit /b 3
)
third_party\ninja\ninja.exe -C out\Grips -j8 chrome
if errorlevel 1 exit /b 4
call python3 "%BYOD_AUDIT_TOOLS%\grips_build_manifest.py" write --root C:\src\chromium\src --out C:\src\chromium\src\out\Grips
if errorlevel 1 exit /b 5
call python3 "%BYOD_AUDIT_TOOLS%\grips_build_manifest.py" verify --root C:\src\chromium\src --out C:\src\chromium\src\out\Grips
exit /b %errorlevel%
