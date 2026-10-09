@echo off
setlocal
set "DEPOT_TOOLS_UPDATE=0"
set "AZURE_CORE_ENABLE_BROKER_ON_WINDOWS=false"
set "BYOD_PACKAGE_TOOLS=C:\src\chromium\byod-google-free-package-20261007"
set "BYOD_SETUP_FILE=C:\Users\mryao\Downloads\BYOD-Browser-Setup-155.0.8059.40-test-signed-20261008.exe"
if exist "%BYOD_PACKAGE_TOOLS%\work" exit /b 20
if exist "%BYOD_SETUP_FILE%" exit /b 21
cd /d C:\src\chromium\src || exit /b 2
call git merge-base --is-ancestor cfaadc5a132d78e1828635aa8405a499f3e14864 HEAD
if errorlevel 1 exit /b 3
call python3 "%BYOD_PACKAGE_TOOLS%\grips_build_manifest.py" verify --root C:\src\chromium\src --out C:\src\chromium\src\out\Grips
if errorlevel 1 exit /b 4
call gn desc out\Grips //chrome:chrome runtime_deps > "%BYOD_PACKAGE_TOOLS%\chrome-runtime-deps.txt"
if errorlevel 1 exit /b 5
call python3 "%BYOD_PACKAGE_TOOLS%\make-byod-installer.py" --root C:\src\chromium\src --out C:\src\chromium\src\out\Grips --output "%BYOD_SETUP_FILE%" --work-dir "%BYOD_PACKAGE_TOOLS%\work" --version 155.0.8059.40 --runtime-deps "%BYOD_PACKAGE_TOOLS%\chrome-runtime-deps.txt" --keep-work --signing-metadata "%BYOD_PACKAGE_TOOLS%\build\azure-signing.metadata.json" --signtool "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" --signing-dlib "C:\Users\mryao\AppData\Local\Microsoft\MicrosoftArtifactSigningClientTools\Azure.CodeSigning.Dlib.dll" --signing-publisher "Bit Precise, LLC"
exit /b %errorlevel%
