@echo off
setlocal
set "TASK_DIR=C:\src\chromium\byod-xhttp-package-20261007"
set "SETUP_FILE=C:\Users\mryao\Downloads\BYOD-Browser-Setup-152.0.7947.0-xhttp-signed-20261007.exe"
if exist "%TASK_DIR%\work" exit /b 20
if exist "%SETUP_FILE%" exit /b 21
cd /d C:\src\chromium\src
set "PATH=C:\src\depot_tools;%PATH%"
call python3 "%TASK_DIR%\make-byod-installer.py" --root C:\src\chromium\src --out C:\src\chromium\src\out\Grips --output "%SETUP_FILE%" --work-dir "%TASK_DIR%\work" --version 152.0.7947.0 --keep-work --signing-metadata "%TASK_DIR%\build\azure-signing.metadata.json" --signtool "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" --signing-dlib "C:\Users\mryao\AppData\Local\Microsoft\MicrosoftArtifactSigningClientTools\Azure.CodeSigning.Dlib.dll" --signing-publisher "Bit Precise, LLC"
exit /b %ERRORLEVEL%
