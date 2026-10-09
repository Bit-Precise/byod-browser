@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem Install (if needed) and launch the installed BYOD Browser.
rem Optional first argument: URL to open. Default is the BYOD exam shell.

set "SCRIPT_DIR=%~dp0"
set "INSTALLER=%BYOD_INSTALLER%"
if not defined INSTALLER if exist "%SCRIPT_DIR%BYOD-Browser-Setup.exe" set "INSTALLER=%SCRIPT_DIR%BYOD-Browser-Setup.exe"
if not defined INSTALLER if exist "%SCRIPT_DIR%BYOD-Browser-Setup-*.exe" (
  for /f "delims=" %%I in ('dir /b /a-d /o-d "%SCRIPT_DIR%BYOD-Browser-Setup-*.exe" 2^>nul') do if not defined INSTALLER set "INSTALLER=%SCRIPT_DIR%%%I"
)

set "BROWSER_DIR=%LOCALAPPDATA%\BYOD Browser"
set "CHROME=%BROWSER_DIR%\chrome.exe"
set "PROFILE=%BROWSER_DIR%\User Data"
set "URL=%~1"
if not defined URL set "URL=grips://exam.cs.ac.cn/"

if not exist "%CHROME%" (
  if not defined INSTALLER (
    echo ERROR: BYOD Browser is not installed and no installer was found beside this BAT.
    echo Put BYOD-Browser-Setup-*.exe in:
    echo   %SCRIPT_DIR%
    goto :fail
  )
  echo Installing BYOD Browser for the current Windows user...
  start "BYOD Browser installer" /wait "%INSTALLER%"
  if errorlevel 1 (
    echo ERROR: installer failed with exit code %ERRORLEVEL%.
    goto :fail
  )
)

if not exist "%CHROME%" (
  echo ERROR: installed browser was not found:
  echo   %CHROME%
  goto :fail
)
if not exist "%PROFILE%" mkdir "%PROFILE%"

echo Starting BYOD Browser...
echo Profile: %PROFILE%
echo URL:     %URL%
start "BYOD Browser" /D "%BROWSER_DIR%" "%CHROME%" ^
  --user-data-dir="%PROFILE%" ^
  --no-first-run ^
  --no-default-browser-check ^
  "%URL%"
goto :done

:fail
if /i not "%~2"=="/nopause" pause
exit /b 1

:done
if /i not "%~2"=="/nopause" pause
exit /b 0
