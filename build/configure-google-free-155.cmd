@echo off
setlocal
set "DEPOT_TOOLS_UPDATE=0"
set "BYOD_UPDATE_TOOLS=C:\src\chromium\byod-upstream-backup-20261007"
cd /d C:\src\chromium\src || exit /b 2
rem Guard against inspecting or building an unintended checkout.
for /f %%H in ('git rev-parse HEAD') do if not "%%H"=="8e5a875bc868009f6ddd4268d26b0c11e3734592" exit /b 3
if not exist "C:\Program Files (x86)\Windows Kits\10\Include\10.0.28000.0\um\Windows.h" exit /b 4
rem Unique recovery snapshot; do not replace an earlier backup or restore logs.
set "BYOD_GN_BACKUP=%BYOD_UPDATE_TOOLS%\gn-backup-%RANDOM%-%RANDOM%"
mkdir "%BYOD_GN_BACKUP%" || exit /b 5
copy out\Grips\.ninja_log "%BYOD_GN_BACKUP%\.ninja_log" || exit /b 6
copy out\Grips\.ninja_deps "%BYOD_GN_BACKUP%\.ninja_deps" || exit /b 7
copy out\Grips\args.gn "%BYOD_GN_BACKUP%\args.gn" || exit /b 8
call gn gen out\Grips
if errorlevel 1 exit /b 9
third_party\ninja\ninja.exe -C out\Grips -n chrome > "%BYOD_UPDATE_TOOLS%\dry-run-155.log" 2>&1
exit /b %errorlevel%
