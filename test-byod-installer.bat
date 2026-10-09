@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem Double-click smoke test for the BYOD Browser installer.
rem The installer is run with an isolated LOCALAPPDATA so this never touches
rem the normal browser profile or the user's existing BYOD Browser install.

set "SCRIPT_DIR=%~dp0"
set "INSTALLER=%BYOD_INSTALLER%"
if not defined INSTALLER if exist "%SCRIPT_DIR%dist\BYOD-Browser-Setup.exe" set "INSTALLER=%SCRIPT_DIR%dist\BYOD-Browser-Setup.exe"
if not defined INSTALLER if exist "%SCRIPT_DIR%dist\BYOD-Browser-Setup-*.exe" (
  for /f "delims=" %%I in ('dir /b /a-d /o-d "%SCRIPT_DIR%dist\BYOD-Browser-Setup-*.exe" 2^>nul') do if not defined INSTALLER set "INSTALLER=%SCRIPT_DIR%dist\%%I"
)
if not defined INSTALLER if exist "%SCRIPT_DIR%BYOD-Browser-Setup.exe" set "INSTALLER=%SCRIPT_DIR%BYOD-Browser-Setup.exe"
if not defined INSTALLER if exist "%SCRIPT_DIR%BYOD-Browser-Setup-*.exe" (
  for /f "delims=" %%I in ('dir /b /a-d /o-d "%SCRIPT_DIR%BYOD-Browser-Setup-*.exe" 2^>nul') do if not defined INSTALLER set "INSTALLER=%SCRIPT_DIR%%%I"
)

if not defined INSTALLER (
  echo ERROR: installer not found.
  echo Put BYOD-Browser-Setup-*.exe in this folder or in .\dist\,
  echo or set BYOD_INSTALLER to its full path before running this BAT.
  goto :fail
)
if not exist "%INSTALLER%" (
  echo ERROR: installer does not exist:
  echo   %INSTALLER%
  goto :fail
)

echo [1/4] Installer: %INSTALLER%

rem Verify the sidecar checksum when one is present.
set "CHECKSUM=%INSTALLER%.sha256"
if exist "%CHECKSUM%" (
  echo [2/4] Verifying SHA-256...
  set "HASH_RESULT="
  for /f "delims=" %%H in ('powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$expected=(Get-Content -Raw -LiteralPath '%CHECKSUM%').Trim().Split()[0].ToLowerInvariant(); $actual=(Get-FileHash -Algorithm SHA256 -LiteralPath '%INSTALLER%').Hash.ToLowerInvariant(); if($expected -eq $actual){'OK'}else{'MISMATCH expected='+$expected+' actual='+$actual}"') do set "HASH_RESULT=%%H"
  if /i not "!HASH_RESULT!"=="OK" (
    echo ERROR: SHA-256 mismatch: !HASH_RESULT!
    goto :fail
  )
  echo       SHA-256 OK
) else (
  echo [2/4] No .sha256 sidecar found; continuing without checksum verification.
)

set "TEST_ROOT=%TEMP%\byod-installer-smoke-%RANDOM%-%RANDOM%"
set "INSTALL_ROOT=%TEST_ROOT%\BYOD Browser"
mkdir "%TEST_ROOT%" 2>nul
if not exist "%TEST_ROOT%" (
  echo ERROR: cannot create isolated test directory:
  echo   %TEST_ROOT%
  goto :fail
)

echo [3/4] Installing into an isolated profile...
set "ORIGINAL_LOCALAPPDATA=%LOCALAPPDATA%"
set "LOCALAPPDATA=%TEST_ROOT%"
start "BYOD Browser installer test" /wait "%INSTALLER%"
set "INSTALLER_RC=%ERRORLEVEL%"
set "LOCALAPPDATA=%ORIGINAL_LOCALAPPDATA%"
if not "%INSTALLER_RC%"=="0" (
  echo ERROR: installer returned exit code %INSTALLER_RC%.
  echo Test files were kept at:
  echo   %TEST_ROOT%
  goto :fail
)

echo [4/4] Verifying installed runtime and build manifest...
for %%F in (chrome.exe chrome.dll resources.pak icudtl.dat v8_context_snapshot.bin byod-build-manifest.json) do (
  if not exist "%INSTALL_ROOT%\%%F" (
    echo ERROR: missing installed file: %%F
    echo Test files were kept at:
    echo   %TEST_ROOT%
    goto :fail
  )
)

set "BYOD_TEST_INSTALL_ROOT=%INSTALL_ROOT%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$root=$env:BYOD_TEST_INSTALL_ROOT; $m=ConvertFrom-Json -InputObject (Get-Content -Raw -LiteralPath (Join-Path $root 'byod-build-manifest.json')); $bad=@(); foreach($n in 'chrome.exe','chrome.dll','resources.pak','icudtl.dat','v8_context_snapshot.bin'){ $p=Join-Path $root $n; $e=$m.files.PSObject.Properties[$n].Value.sha256; if((-not $e) -or ((Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash -ne $e)){$bad+=$n} }; if($bad.Count){Write-Host ('manifest mismatch: '+($bad -join ', ')); exit 1}"
if errorlevel 1 (
  echo ERROR: installed files do not match the build manifest.
  echo Test files were kept at:
  echo   %TEST_ROOT%
  goto :fail
)

set "VERSION=unknown"
if exist "%INSTALL_ROOT%\byod-browser.version" set /p VERSION=<"%INSTALL_ROOT%\byod-browser.version"
echo SUCCESS: BYOD Browser installer test passed.
echo Installed version: %VERSION%
echo Isolated install:  %INSTALL_ROOT%
if /i "%~1"=="/cleanup" (
  rmdir /s /q "%TEST_ROOT%"
  echo Cleaned up the isolated install.
)
goto :done

:fail
set "RESULT=1"
if /i "%~1"=="/cleanup" if defined TEST_ROOT if exist "%TEST_ROOT%" rmdir /s /q "%TEST_ROOT%"
if not defined RESULT set "RESULT=1"
if /i not "%~1"=="/nopause" if /i not "%~2"=="/nopause" pause
exit /b %RESULT%

:done
if /i not "%~1"=="/nopause" if /i not "%~2"=="/nopause" pause
exit /b 0
