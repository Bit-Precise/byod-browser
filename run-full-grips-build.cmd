@echo off
setlocal
set "ROOT=C:\src\chromium\src"
set "OUT=%ROOT%\out\Grips"
set "LOG=C:\src\chromium\full-clean-Grips-build.log"
set "MANIFEST=%OUT%\byod-build-manifest.json"
set "MANIFEST_TOOL=%~dp0grips_build_manifest.py"
cd /d "%ROOT%" || exit /b 2
if not exist "%MANIFEST_TOOL%" (
  echo ERROR: manifest tool is missing: "%MANIFEST_TOOL%"
  exit /b 3
)

rem Invalidate the previous success marker before Ninja starts. A failed or
rem interrupted build must never be launched as if it were complete.
if exist "%MANIFEST%" del /f /q "%MANIFEST%" >nul

echo BUILD_START %date% %time% >> "%LOG%"
third_party\ninja\ninja.exe -C "%OUT%" -j32 chrome >> "%LOG%" 2>&1
set "RC=%errorlevel%"
echo BUILD_EXIT %RC% %date% %time% >> "%LOG%"
if not "%RC%"=="0" exit /b %RC%

where python3 >nul 2>&1
if errorlevel 1 (set "PYTHON=python") else (set "PYTHON=python3")
"%PYTHON%" "%MANIFEST_TOOL%" write --root "%ROOT%" --out "%OUT%"
if errorlevel 1 (
  echo ERROR: build succeeded but the integrity manifest could not be written.
  exit /b 4
)
exit /b %RC%
