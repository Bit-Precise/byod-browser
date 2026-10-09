@echo off
setlocal
cd /d C:\src\chromium\src || exit /b 2
if not exist C:\src\chromium\byod-upstream-backup-20261007\snapshot.json exit /b 3
rem Local recovery checkpoint only; no push, reset, stash, or cleanup.
for /f %%H in ('git rev-parse HEAD') do if not "%%H"=="c2d350cd776d4024caee095b6cee010613bf02c9" exit /b 9
call git switch byod/snapshot-20261007
if errorlevel 1 exit /b 4
call git add -u
if errorlevel 1 exit /b 5
call git add chrome/browser/resources/grips/BUILD.gn chrome/browser/resources/grips/bridge.css chrome/browser/resources/grips/bridge.html chrome/browser/resources/grips/bridge.ts chrome/browser/resources/grips/login.html chrome/browser/resources/grips/login.ts chrome/browser/ui/webui/grips/BUILD.gn chrome/browser/ui/webui/grips/grips_ui.h chrome/browser/ui/webui/grips/grips_ui_browsertest.cc chrome/browser/ui/webui/grips/grips_url_handler.cc chrome/browser/ui/webui/grips/grips_url_handler.h content/public/browser/byod_navigation_audit.h
if errorlevel 1 exit /b 6
call git -c user.name="BYOD Local Build" -c user.email="build@byod.invalid" commit -m "snapshot: preserve BYOD integration before Chromium 155 update"
if errorlevel 1 exit /b 7
call git switch -c byod/google-free-155-20261007
if errorlevel 1 exit /b 8
call git log -1 --format=oneline
exit /b %errorlevel%
