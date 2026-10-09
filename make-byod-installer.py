#!/usr/bin/env python3
"""Build a small, per-user BYOD Browser installer for Windows.

The Chromium mini-installer is useful for a normal Chrome installation, but it
does not create a BYOD-specific launcher or profile. This script packages the
runtime from one *single* Chromium output directory, verifies it against the
build manifest, and wraps it in an IExpress self-extractor.

Typical usage on the Windows build host::

    py -3 make-byod-installer.py \
        --root C:\\src\\chromium\\src \
        --out C:\\src\\chromium\\src\\out\\Grips \
        --output C:\\Users\\mryao\\Downloads\\BYOD-Browser-Setup.exe

The default paths retain the layout used by the project's remote Windows
build host. The generated installer is per-user and does not require UAC: it
installs to ``%LOCALAPPDATA%\\BYOD Browser`` and creates a dedicated user data
directory below ``%LOCALAPPDATA%``.
"""

from __future__ import annotations

import argparse
import copy
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import zipfile


DEFAULT_ROOT = Path(r"C:\src\chromium\src")
DEFAULT_OUT = DEFAULT_ROOT / "out" / "Grips"
DEFAULT_OUTPUT = Path(r"C:\Users\mryao\Downloads\BYOD-Browser-Setup.exe")

REQUIRED_FILES = (
    "chrome.exe",
    "chrome.dll",
    "resources.pak",
    "icudtl.dat",
    "v8_context_snapshot.bin",
)

# These are the directories shipped by Chromium when they exist in the output
# directory. We deliberately do not copy arbitrary directories: an output
# tree also contains obj/, test data, symbols, and other non-runtime files.
RUNTIME_DIRECTORIES = (
    "angledata",
    "locales",
    "resources",
    "swiftshader",
    "MEIPreload",
    "hyphen-data",
    "extensions",
    "IwaKeyDistribution",
    "PrivacySandboxAttestationsPreloaded",
    "WidevineCdm",
    "FileTypePolicies",
)

# Top-level files needed by a Chromium distribution. DLL/PAK/BIN/DAT files
# are runtime inputs; the explicit names cover the few files with other
# extensions. Symbols, import libraries, test executables, and *.info files
# are intentionally excluded.
RUNTIME_SUFFIXES = {".dll", ".pak", ".bin", ".dat", ".manifest"}
RUNTIME_FILES = {
    "chrome.exe",
    "chrome.VisualElementsManifest.xml",
    "byod-build-manifest.json",
    "snapshot_blob.bin",
    "v8_build_config.json",
    "v8_features.json",
    "vk_swiftshader_icd.json",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT,
                        help="Chromium checkout (used for diagnostics)")
    parser.add_argument("--out", type=Path, default=None,
                        help="Chromium output directory (default: ROOT/out/Grips)")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT,
                        help="generated self-extracting .exe")
    parser.add_argument("--work-dir", type=Path, default=None,
                        help="temporary packaging directory (default: beside output)")
    parser.add_argument("--version", default=None,
                        help="version shown in installer metadata (default: manifest/source version)")
    parser.add_argument("--runtime-deps", type=Path,
                        help="current GN chrome runtime_deps list; excludes stale outputs")
    parser.add_argument("--keep-work", action="store_true",
                        help="keep the staging directory for inspection")
    parser.add_argument("--no-launch", action="store_true",
                        help="do not start BYOD Browser automatically after installation")
    parser.add_argument("--signing-metadata", type=Path,
                        help="Azure Artifact Signing metadata JSON (enables signing)")
    parser.add_argument("--signtool", type=Path, help="x64 Windows SDK signtool.exe")
    parser.add_argument("--signing-dlib", type=Path, help="x64 Azure.CodeSigning.Dlib.dll")
    parser.add_argument("--signing-publisher", help="expected certificate publisher name")
    args = parser.parse_args()
    signing_options = (args.signing_metadata, args.signtool,
                       args.signing_dlib, args.signing_publisher)
    if any(signing_options) and not all(signing_options):
        parser.error("signing requires --signing-metadata, --signtool, "
                     "--signing-dlib and --signing-publisher together")
    if args.out is None:
        args.out = args.root / "out" / "Grips"
    return args


def verify_build(out: Path) -> dict:
    """Verify that all packaged files came from the same canonical build."""
    manifest_path = out / "byod-build-manifest.json"
    if not manifest_path.is_file():
        raise RuntimeError(f"missing build manifest: {manifest_path}")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"cannot read build manifest {manifest_path}: {exc}") from exc

    recorded_files = manifest.get("files", {})
    for name in REQUIRED_FILES:
        path = out / name
        if not path.is_file():
            raise RuntimeError(f"missing runtime file: {path}")
        expected = recorded_files.get(name, {}).get("sha256")
        if not expected:
            raise RuntimeError(f"manifest has no SHA-256 for {name}")
        actual = sha256(path)
        if actual != expected:
            raise RuntimeError(
                f"manifest mismatch for {name}: expected {expected}, got {actual}"
            )

    manifest_out = manifest.get("out")
    if manifest_out and Path(manifest_out).resolve() != out.resolve():
        raise RuntimeError(
            f"manifest belongs to {manifest_out}, not the requested output {out}"
        )
    return manifest


def runtime_files(out: Path) -> list[Path]:
    files: list[Path] = []
    for path in sorted(out.iterdir(), key=lambda item: item.name.lower()):
        if not path.is_file():
            continue
        if path.name in RUNTIME_FILES or path.suffix.lower() in RUNTIME_SUFFIXES:
            files.append(path)
    for name in REQUIRED_FILES:
        path = out / name
        if path not in files:
            files.append(path)
    return sorted(set(files), key=lambda item: item.name.lower())


def declared_runtime_files(out: Path, dependencies: Path) -> list[Path]:
    """Select distribution files from the current target, not leftover outputs.

    GN also lists build-time DevTools sources and PDBs. Retain the distribution
    layout supported below; never copy gen/, obj/, or files outside out/.
    """
    names = {"byod-build-manifest.json"}
    for line in dependencies.read_text(encoding="utf-8-sig").splitlines():
        if not line.strip():
            continue
        path = PurePosixPath(line.strip().replace("\\", "/"))
        if path.is_absolute() or ".." in path.parts or ":" in str(path):
            raise RuntimeError(f"unsafe runtime dependency: {line}")
        if (len(path.parts) == 1 and
                (path.name in RUNTIME_FILES or
                 path.suffix.lower() in RUNTIME_SUFFIXES | {".exe"})):
            names.add(path.as_posix())
        elif len(path.parts) > 1 and path.parts[0] in RUNTIME_DIRECTORIES:
            names.add(path.as_posix())
    missing = set(REQUIRED_FILES) - names
    if missing:
        raise RuntimeError("GN runtime dependencies omit required files: " +
                           ", ".join(sorted(missing)))
    files = []
    for name in sorted(names):
        source = out / name
        if not source.resolve().is_relative_to(out.resolve()):
            raise RuntimeError(f"runtime dependency leaves output directory: {name}")
        if not source.is_file():
            raise RuntimeError(f"missing declared runtime file: {name}")
        files.append(source)
    return files


def copy_runtime(out: Path, stage: Path, dependencies: Path | None = None) -> None:
    stage.mkdir(parents=True, exist_ok=True)
    if dependencies:
        for source in declared_runtime_files(out, dependencies):
            target = stage / source.relative_to(out)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        return
    for path in runtime_files(out):
        shutil.copy2(path, stage / path.name)
    for name in RUNTIME_DIRECTORIES:
        source = out / name
        if source.is_dir():
            shutil.copytree(source, stage / name)


def copy_license_notices(root: Path, stage: Path) -> None:
    """Ship source and reused patch licenses inside the signed payload."""
    chromium_license = root / "LICENSE"
    if not chromium_license.is_file():
        raise RuntimeError(f"missing Chromium license: {chromium_license}")
    shutil.copy2(chromium_license, stage / "LICENSE.chromium.txt")
    if (root / "net/traffic_annotation/byod_google_services.h").is_file():
        patch_license = (Path(__file__).resolve().parent / "patches" /
                         "google-services" / "LICENSE.ungoogled-chromium")
        if not patch_license.is_file():
            raise RuntimeError(f"missing service-removal patch license: {patch_license}")
        shutil.copy2(patch_license, stage / "LICENSE.ungoogled-chromium.txt")


def write_launcher(stage: Path, version: str, launch_after_install: bool) -> None:
    # This script is run by IExpress from its extraction directory. The
    # PowerShell helper performs the actual extraction and displays a native
    # progress window; it is also included in the managed self-extractor
    # fallback package.
    launch = (
        'start "BYOD Browser" "%DEST%\\chrome.exe" '
        '--no-first-run '
        '--no-default-browser-check "grips://exam.cs.ac.cn/"'
        if launch_after_install else "rem launch disabled by --no-launch"
    )
    install = f'''@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "DEST=%LOCALAPPDATA%\\BYOD Browser"
set "USER_DATA=%LOCALAPPDATA%\\BYOD Browser\\User Data"
echo [BYOD Browser] Starting installer...
if not exist "%DEST%" mkdir "%DEST%"
if errorlevel 1 exit /b 1
if not exist "%USER_DATA%" mkdir "%USER_DATA%"
if errorlevel 1 exit /b 1
if exist "%~dp0BYOD-Browser-payload.zip" (
  echo [BYOD Browser] Installing runtime files...
  powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0install-progress.ps1" -Payload "%~dp0BYOD-Browser-payload.zip" -Destination "%DEST%"
  if not errorlevel 1 goto :payload_installed
  rem Fall back to the native extractor if the progress UI cannot start.
  tar.exe -xf "%~dp0BYOD-Browser-payload.zip" -C "%DEST%"
  if errorlevel 1 exit /b 1
) else (
  rem The native self-extractor fallback expands the staged directory directly.
  robocopy "%~dp0." "%DEST%" /E /NFL /NDL /NJH /NJS /NP /XF install.cmd payload.zip byod-browser.version >nul
  if errorlevel 8 exit /b 1
)
:payload_installed
rem Chromium's Windows sandbox needs read/execute access through the
rem application package capability SIDs. Keep the profile ACL private.
icacls "%DEST%" /grant "*S-1-15-2-1:(OI)(CI)(RX)" "*S-1-15-2-2:(OI)(CI)(RX)" /T >nul 2>&1
icacls "%USER_DATA%" /inheritance:r /T >nul 2>&1
icacls "%USER_DATA%" /grant "%USERNAME%:(OI)(CI)(F)" "*S-1-5-18:(OI)(CI)(F)" "*S-1-5-32-544:(OI)(CI)(F)" /T >nul 2>&1
>"%DEST%\\start-byod-exam.cmd" echo @echo off
>>"%DEST%\\start-byod-exam.cmd" echo setlocal
>>"%DEST%\\start-byod-exam.cmd" echo start "BYOD Browser" "%%~dp0chrome.exe" --no-first-run --no-default-browser-check "grips://exam.cs.ac.cn/"
>>"%DEST%\\start-byod-exam.cmd" echo endlocal
>"%DEST%\\uninstall-byod-browser.cmd" echo @echo off
>>"%DEST%\\uninstall-byod-browser.cmd" echo echo This removes BYOD Browser from %%LOCALAPPDATA%%.
>>"%DEST%\\uninstall-byod-browser.cmd" echo choice /M "Continue"
>>"%DEST%\\uninstall-byod-browser.cmd" echo if errorlevel 2 exit /b 0
>>"%DEST%\\uninstall-byod-browser.cmd" echo rmdir /s /q "%%~dp0"

rem Create Start Menu and Desktop shortcuts when PowerShell is available.
rem A Windows shortcut stores its target and raw argument string separately.
rem Build embedded quotes with [char]34 so cmd.exe cannot consume them before
rem WScript.Shell writes the .lnk Arguments field.
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ws=New-Object -ComObject WScript.Shell; $q=[char]34; $args='--no-first-run --no-default-browser-check '+$q+'grips://exam.cs.ac.cn/'+$q; foreach($folder in @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Programs'))) {{ $p=Join-Path $folder 'BYOD Browser.lnk'; $s=$ws.CreateShortcut($p); $s.TargetPath='%DEST%\\chrome.exe'; $s.Arguments=$args; $s.WorkingDirectory='%DEST%'; $s.IconLocation='%DEST%\\chrome.exe,0'; $s.Description='BYOD Browser'; $s.Save() }}" >nul 2>&1
>"%DEST%\\byod-browser.version" echo {version}
{launch}
exit /b 0
'''
    (stage.parent / "install.cmd").write_text(install, encoding="utf-8", newline="\r\n")
    (stage / "install-progress.ps1").write_text(r'''param(
  [Parameter(Mandatory = $true)] [string]$Payload,
  [Parameter(Mandatory = $true)] [string]$Destination
)

$ErrorActionPreference = "Stop"
$form = $null
$zip = $null
$payloadStream = $null
$errorLog = Join-Path ([System.IO.Path]::GetTempPath()) "BYOD-Browser-install-error.log"

try {
  Add-Type -AssemblyName System.Windows.Forms
  Add-Type -AssemblyName System.Drawing
  Add-Type -AssemblyName System.IO.Compression
  Add-Type -AssemblyName System.IO.Compression.FileSystem

  $form = New-Object System.Windows.Forms.Form
  $form.Text = "BYOD Browser"
  $form.Width = 520
  $form.Height = 145
  $form.StartPosition = "CenterScreen"
  $form.FormBorderStyle = "FixedDialog"
  $form.MaximizeBox = $false
  $form.MinimizeBox = $false
  $form.ControlBox = $false

  $status = New-Object System.Windows.Forms.Label
  $status.Left = 18
  $status.Top = 16
  $status.Width = 468
  $status.Height = 34
  $status.Text = "Preparing installation..."
  $form.Controls.Add($status)

  $progress = New-Object System.Windows.Forms.ProgressBar
  $progress.Left = 18
  $progress.Top = 65
  $progress.Width = 468
  $progress.Height = 24
  $progress.Minimum = 0
  $progress.Maximum = 100
  $progress.Style = "Continuous"
  $form.Controls.Add($progress)
  $form.Show()
  [System.Windows.Forms.Application]::DoEvents()

  $destinationRoot = [System.IO.Path]::GetFullPath($Destination)
  if (-not $destinationRoot.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
    $destinationRoot += [System.IO.Path]::DirectorySeparatorChar
  }
  $installedChrome = [System.IO.Path]::Combine($destinationRoot, "chrome.exe")
  function Get-InstalledBrowserProcesses {
    @(Get-CimInstance Win32_Process -Filter "Name = 'chrome.exe'" -ErrorAction SilentlyContinue | Where-Object {
      $_.ExecutablePath -and ([System.String]::Equals([System.IO.Path]::GetFullPath($_.ExecutablePath), $installedChrome, [System.StringComparison]::OrdinalIgnoreCase))
    })
  }
  $runningChrome = @(Get-InstalledBrowserProcesses)
  if ($runningChrome.Count -gt 0) {
    $status.Text = "Closing BYOD Browser..."
    [System.Windows.Forms.Application]::DoEvents()
    foreach ($process in $runningChrome) {
      try {
        Start-Process -FilePath "taskkill.exe" -ArgumentList @('/PID', [string]$process.ProcessId, '/T', '/F') -Wait -WindowStyle Hidden
      } catch {
        try { Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue } catch { }
      }
    }
    for ($wait = 0; $wait -lt 20; $wait++) {
      if (@(Get-InstalledBrowserProcesses).Count -eq 0) { break }
      Start-Sleep -Milliseconds 250
      [System.Windows.Forms.Application]::DoEvents()
    }
    $remaining = @(Get-InstalledBrowserProcesses)
    if ($remaining.Count -gt 0) {
      $owners = ($remaining | ForEach-Object { "PID $($_.ProcessId): $($_.ExecutablePath)" }) -join "`n"
      throw ("Could not close all BYOD Browser processes:`n`n" + $owners)
    }
  }
  $payloadStream = [System.IO.File]::OpenRead([System.IO.Path]::GetFullPath($Payload))
  $zip = New-Object -TypeName System.IO.Compression.ZipArchive -ArgumentList @($payloadStream, ([System.IO.Compression.ZipArchiveMode]::Read), $false)
  $total = [int64](($zip.Entries | Measure-Object -Property Length -Sum).Sum)
  if ($total -le 0) { $total = 1 }
  [int64]$copied = 0
  $buffer = New-Object byte[] (1024 * 1024)

  function Open-InstallerOutput([string]$Path) {
    for ($attempt = 1; $attempt -le 20; $attempt++) {
      try {
        # Allow security software to inspect a file while it is being copied;
        # retry briefly if it has a transient handle during an update.
        return [System.IO.File]::Open($Path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
      } catch {
        if ($attempt -eq 20) {
          throw "Cannot replace $Path. Another process or security scanner is using the file. Close BYOD Browser and retry."
        }
        Start-Sleep -Milliseconds 500
      }
    }
  }

  foreach ($entry in $zip.Entries) {
    $relative = $entry.FullName.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
    $target = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($destinationRoot, $relative))
    if (-not $target.StartsWith($destinationRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
      throw "The installer payload contains an unsafe path."
    }
    if ($entry.FullName.EndsWith('/')) {
      [System.IO.Directory]::CreateDirectory($target) | Out-Null
      continue
    }

    $parent = [System.IO.Path]::GetDirectoryName($target)
    if ($parent) { [System.IO.Directory]::CreateDirectory($parent) | Out-Null }
    $entryStream = $null
    $outputStream = $null
    try {
      $entryStream = $entry.Open()
      $outputStream = Open-InstallerOutput $target
      while (($read = $entryStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
        $outputStream.Write($buffer, 0, $read)
        $copied += $read
        $value = [Math]::Min(100, [int](($copied * 100) / $total))
        $progress.Value = $value
        $status.Text = "Installing " + $entry.Name
        [System.Windows.Forms.Application]::DoEvents()
      }
    } finally {
      if ($outputStream) { $outputStream.Dispose() }
      if ($entryStream) { $entryStream.Dispose() }
    }
  }
  $progress.Value = 100
  $status.Text = "Finishing installation..."
  [System.Windows.Forms.Application]::DoEvents()
  Start-Sleep -Milliseconds 250
  $form.Close()
  exit 0
} catch {
  try {
    (Get-Date -Format o) + " " + $_.Exception.ToString() | Set-Content -LiteralPath $errorLog -Encoding UTF8
  } catch { }
  try {
    if ($form) { $form.Close() }
    [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, "BYOD Browser installation failed", "OK", "Error") | Out-Null
  } catch { }
  exit 1
} finally {
  if ($zip) { $zip.Dispose() }
  if ($payloadStream) { $payloadStream.Dispose() }
}
''', encoding="utf-8", newline="\r\n")


def write_sed(work: Path, output: Path, payload: Path, version: str) -> Path:
    # Use explicit source paths. This works with both the GUI and quiet
    # IExpress modes and avoids ambiguity when the work directory is elsewhere.
    sed = work / "setup.sed"
    sed.write_text(
        f'''[Version]
Class=IEXPRESS
SEDVersion=3
[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=1
HideExtractAnimation=0
HidePrompt=1
SourceFiles=1
AppLaunched=install.cmd
TargetName={output}
FriendlyName=BYOD Browser {version}
InstallPrompt=
DisplayLicense=
FinishMessage=BYOD Browser {version} installed for the current user.
[Strings]
FILE0="install.cmd"
FILE1="BYOD-Browser-payload.zip"
FILE2="install-progress.ps1"
[SourceFiles]
SourceFiles0={work}
[SourceFiles0]
%FILE0%={work / 'install.cmd'}
%FILE1%={payload}
%FILE2%={work / 'stage' / 'install-progress.ps1'}
''',
        encoding="utf-8",
        newline="\r\n",
    )
    return sed


def write_zip_payload(stage: Path, work: Path) -> Path:
    """Create a ZIP used by the compiler fallback self-extractor."""
    payload = work / "BYOD-Browser-payload.zip"
    with zipfile.ZipFile(payload, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in sorted(stage.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(stage).as_posix())
    return payload


def build_csharp_self_extractor(payload: Path, output: Path) -> None:
    """Build a dependency-free Windows self-extractor when IExpress fails.

    Some Windows images ship iexpress.exe but disable its cabinet builder.
    The .NET Framework compiler is part of the same image, so embedding the
    ZIP in a tiny managed EXE keeps the installer usable without downloading
    a third-party packager.
    """
    csc = shutil.which("csc.exe")
    if not csc:
        candidate = Path(r"C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe")
        if candidate.is_file():
            csc = str(candidate)
    if not csc:
        raise RuntimeError("IExpress failed and csc.exe was not found for fallback packaging")
    source = payload.parent / "byod_installer_stub.cs"
    source.write_text(r'''using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;

internal static class ByodInstaller {
  public static int Main() {
    string temp = Path.Combine(Path.GetTempPath(), "BYOD-Browser-" + Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(temp);
    string zip = Path.Combine(temp, "BYOD-Browser-payload.zip");
    string resource = null;
    foreach (string name in Assembly.GetExecutingAssembly().GetManifestResourceNames()) {
      if (name.EndsWith("ByodPayload.zip", StringComparison.OrdinalIgnoreCase)) { resource = name; break; }
    }
    if (resource == null) throw new InvalidOperationException("embedded payload is missing");
    using (Stream input = Assembly.GetExecutingAssembly().GetManifestResourceStream(resource))
    using (FileStream output = File.Create(zip)) input.CopyTo(output);
    string command = "-NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" + zip.Replace("'", "''") + "' -DestinationPath '" + temp.Replace("'", "''") + "' -Force\"";
    Process expand = Process.Start(new ProcessStartInfo("powershell.exe", command) { UseShellExecute = false, CreateNoWindow = true });
    expand.WaitForExit();
    if (expand.ExitCode != 0) return expand.ExitCode;
    string installPath = Path.Combine(temp, "install.cmd");
    Process install = Process.Start(new ProcessStartInfo("cmd.exe", "/d /c \"\"" + installPath + "\"\"") { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = temp });
    install.WaitForExit();
    int result = install.ExitCode;
    try { Directory.Delete(temp, true); } catch { }
    return result;
  }
}
''', encoding="utf-8", newline="\r\n")
    subprocess.run([
        csc, "/nologo", "/target:winexe", "/optimize+",
        f"/out:{output}", f"/resource:{payload},ByodPayload.zip", str(source),
    ], check=True)


def version_for(manifest: dict, requested: str | None) -> str:
    if requested:
        return requested
    # Chromium's version is not part of the manifest schema, so retain a
    # stable readable fallback while allowing a build job to pass --version.
    value = manifest.get("version") or manifest.get("chrome_version")
    return str(value) if value else "development"


def sign_path(path: Path, args: argparse.Namespace) -> None:
    helper = Path(__file__).resolve().parent / "build" / "sign-windows-artifacts.ps1"
    for required in (helper, args.signing_metadata, args.signtool, args.signing_dlib):
        if not required.is_file():
            raise RuntimeError(f"missing signing input: {required}")
    subprocess.run([
        "powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
        "-File", str(helper), "-Path", str(path),
        "-Metadata", str(args.signing_metadata.resolve()),
        "-SignTool", str(args.signtool.resolve()),
        "-Dlib", str(args.signing_dlib.resolve()),
        "-Publisher", args.signing_publisher,
    ], check=True)


def sign_staged_runtime(stage: Path, manifest: dict, args: argparse.Namespace) -> None:
    # Validate the copies before changing them; never sign the canonical out/ tree.
    for name in REQUIRED_FILES:
        if sha256(stage / name) != manifest["files"][name]["sha256"]:
            raise RuntimeError(f"staged runtime changed before signing: {name}")
    original_manifest_hash = sha256(stage / "byod-build-manifest.json")
    sign_path(stage, args)
    packaged = copy.deepcopy(manifest)
    packaged["build_files"] = copy.deepcopy(manifest["files"])
    for path in sorted(stage.rglob("*")):
        if not path.is_file():
            continue
        name = path.relative_to(stage).as_posix()
        if name in packaged["files"] or path.suffix.lower() in {".exe", ".dll"}:
            stat = path.stat()
            packaged["files"][name] = {
                "sha256": sha256(path), "size": stat.st_size, "mtime_ns": stat.st_mtime_ns,
            }
    metadata = json.loads(args.signing_metadata.read_text(encoding="utf-8-sig"))
    packaged["signing"] = {
        "provider": "Azure Artifact Signing",
        "endpoint": metadata["Endpoint"],
        "account": metadata["CodeSigningAccountName"],
        "certificate_profile": metadata["CertificateProfileName"],
        "publisher": args.signing_publisher,
        "packaged_utc": datetime.now(timezone.utc).isoformat(),
        "source_manifest_sha256": original_manifest_hash,
    }
    (stage / "byod-build-manifest.json").write_text(
        json.dumps(packaged, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    out = args.out.resolve()
    output = args.output.resolve()
    work = (args.work_dir or output.parent / "byod-installer-work").resolve()
    stage = work / "stage"
    payload = work / "BYOD-Browser-payload.zip"

    try:
        manifest = verify_build(out)
        version = version_for(manifest, args.version)
        tar = shutil.which("tar.exe") or shutil.which("tar")
        iexpress = shutil.which("iexpress.exe") or shutil.which("iexpress")
        if not tar:
            raise RuntimeError("tar.exe was not found; install/use Windows 10 or newer")
        if not iexpress:
            raise RuntimeError("iexpress.exe was not found (Windows component missing)")

        if work.exists():
            shutil.rmtree(work)
        stage.mkdir(parents=True)
        copy_runtime(out, stage, args.runtime_deps)
        copy_license_notices(args.root.resolve(), stage)
        write_launcher(stage, version, not args.no_launch)
        shutil.copy2(work / "install.cmd", stage / "install.cmd")
        if args.signing_metadata:
            sign_staged_runtime(stage, manifest, args)

        # Keep only the two files consumed by IExpress in its source root.
        # write_launcher() already placed install.cmd beside the stage.
        subprocess.run(
            [tar, "-a", "-c", "-f", str(payload), "-C", str(stage), "."],
            check=True,
        )
        sed = write_sed(work, output, payload, version)
        output.parent.mkdir(parents=True, exist_ok=True)
        if output.exists():
            output.unlink()
        try:
            subprocess.run([iexpress, "/N", "/Q", str(sed)], check=True)
        except subprocess.CalledProcessError as exc:
            # IExpress is present on some stripped-down Windows images but its
            # cabinet builder is disabled. Fall back to a managed self-
            # extractor, which is still a single EXE and needs no third-party
            # installer runtime.
            print(f"[info] IExpress failed ({exc}); using .NET self-extractor")
            zip_payload = write_zip_payload(stage, work)
            build_csharp_self_extractor(zip_payload, output)
        if not output.is_file() or output.stat().st_size == 0:
            raise RuntimeError(f"IExpress did not create {output}")
        if args.signing_metadata:
            sign_path(output, args)
            # Hash the final signed installer, not the unsigned intermediate.
            Path(str(output) + ".sha256").write_text(
                f"{sha256(output)}  {output.name}\n", encoding="ascii")

        print(f"SETUP={output}")
        print(f"SIZE={output.stat().st_size}")
        print(f"SHA256={sha256(output)}")
        print(f"PAYLOAD={payload}")
        return 0
    except (OSError, subprocess.CalledProcessError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    finally:
        if work.exists() and not args.keep_work:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
