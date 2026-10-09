@echo off
setlocal
set "DEPOT_TOOLS_UPDATE=0"
set "BYOD_UPDATE_TOOLS=C:\src\chromium\byod-upstream-backup-20261007"
cd /d C:\src\chromium\src || exit /b 2
for /f %%H in ('git rev-parse HEAD') do if not "%%H"=="8e5a875bc868009f6ddd4268d26b0c11e3734592" exit /b 3
call git apply --check "%BYOD_UPDATE_TOOLS%\1400-service-removal.patch"
if errorlevel 1 exit /b 4
call git apply "%BYOD_UPDATE_TOOLS%\1400-service-removal.patch"
if errorlevel 1 exit /b 5
call git diff --check
if errorlevel 1 exit /b 6
rem Preserve incremental state before GN. No cleaning, no restored old logs.
if not exist "%BYOD_UPDATE_TOOLS%\pre-google-gn" mkdir "%BYOD_UPDATE_TOOLS%\pre-google-gn"
copy /-Y out\Grips\.ninja_log "%BYOD_UPDATE_TOOLS%\pre-google-gn\.ninja_log" || exit /b 7
copy /-Y out\Grips\.ninja_deps "%BYOD_UPDATE_TOOLS%\pre-google-gn\.ninja_deps" || exit /b 8
copy /-Y out\Grips\args.gn "%BYOD_UPDATE_TOOLS%\pre-google-gn\args.gn" || exit /b 9
copy /Y "%BYOD_UPDATE_TOOLS%\google-free-args.gn" out\Grips\args.gn || exit /b 10
call gn gen out\Grips
if errorlevel 1 exit /b 11
third_party\ninja\ninja.exe -C out\Grips -n chrome > "%BYOD_UPDATE_TOOLS%\dry-run-155.log" 2>&1
exit /b %errorlevel%
