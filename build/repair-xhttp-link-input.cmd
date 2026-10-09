@echo off
setlocal
cd /d C:\src\chromium\src || exit /b 2
rem Confirmed corrupt COFF input. Preserve it; rebuild only this exact object.
set "BYOD_BAD_OBJECT=out\Grips\obj\third_party\blink\renderer\modules\accessibility\accessibility\ax_relation_cache.obj"
set "BYOD_SAVED_OBJECT=C:\src\chromium\byod-xhttp-tools-20261007\ax_relation_cache.obj.corrupt"
if not exist "%BYOD_BAD_OBJECT%" exit /b 3
if exist "%BYOD_SAVED_OBJECT%" exit /b 4
move "%BYOD_BAD_OBJECT%" "%BYOD_SAVED_OBJECT%" >nul || exit /b 5
third_party\ninja\ninja.exe -C out\Grips -n obj/third_party/blink/renderer/modules/accessibility/accessibility/ax_relation_cache.obj
exit /b %errorlevel%
