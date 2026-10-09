@echo off
setlocal
set "DEPOT_TOOLS_UPDATE=0"
set "BYOD_UPDATE_TOOLS=C:\src\chromium\byod-upstream-backup-20261007"
cd /d C:\src\chromium\src || exit /b 2
for /f %%H in ('git rev-parse HEAD') do if not "%%H"=="8e5a875bc868009f6ddd4268d26b0c11e3734592" exit /b 3
if not exist "%BYOD_UPDATE_TOOLS%\dry-run-155-full.log" exit /b 4
rem Include the new generated guard in the source fingerprint, without staging
rem its contents or committing any source changes.
call git add -N net/traffic_annotation/byod_google_services.h
if errorlevel 1 exit /b 5
if exist out\Grips\byod-build-manifest.json (
  move out\Grips\byod-build-manifest.json "%BYOD_UPDATE_TOOLS%\manifest-before-build-%RANDOM%-%RANDOM%.json" >nul
  if errorlevel 1 exit /b 6
)
echo BUILD_START %date% %time%
rem Keep compiling independent targets after an error, then return failure.
rem This only controls error collection; warning/build flags are unchanged.
third_party\ninja\ninja.exe -C out\Grips -j8 -k 0 chrome
set "BYOD_BUILD_RC=%errorlevel%"
echo BUILD_EXIT %BYOD_BUILD_RC% %date% %time%
if not "%BYOD_BUILD_RC%"=="0" exit /b %BYOD_BUILD_RC%
call python3 "%BYOD_UPDATE_TOOLS%\grips_build_manifest.py" write --root C:\src\chromium\src --out C:\src\chromium\src\out\Grips
if errorlevel 1 exit /b 7
call python3 "%BYOD_UPDATE_TOOLS%\grips_build_manifest.py" verify --root C:\src\chromium\src --out C:\src\chromium\src\out\Grips
exit /b %errorlevel%
