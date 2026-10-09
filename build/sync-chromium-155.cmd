@echo off
setlocal
set "DEPOT_TOOLS_UPDATE=0"
cd /d C:\src\chromium\src || exit /b 2
for /f %%H in ('git rev-parse HEAD') do if not "%%H"=="8e5a875bc868009f6ddd4268d26b0c11e3734592" exit /b 3
cd /d C:\src\chromium || exit /b 4
rem Root is unmanaged: synchronize DEPS without replacing the BYOD branch.
rem Do not use reset, force, delete_unversioned_trees, or clean.
call gclient sync --nohooks --jobs=8
if errorlevel 1 exit /b 5
cd /d C:\src\chromium\src || exit /b 6
for /f %%H in ('git rev-parse HEAD') do if not "%%H"=="8e5a875bc868009f6ddd4268d26b0c11e3734592" exit /b 7
exit /b 0
