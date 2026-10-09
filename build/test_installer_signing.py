"""Offline tests: signing must never mutate the canonical build inputs."""

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "installer", Path(__file__).resolve().parents[1] / "make-byod-installer.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class SigningTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix="byod-signing-test-")
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        self.out = self.root / "original"
        self.stage = self.root / "stage"
        self.out.mkdir()
        for name in installer.REQUIRED_FILES:
            (self.out / name).write_bytes(name.encode())
        (self.out / "extra.dll").write_bytes(b"extra")
        self.manifest = {
            "out": str(self.out), "source_revision": "test-revision",
            "files": {name: {"sha256": installer.sha256(self.out / name)}
                      for name in installer.REQUIRED_FILES},
        }
        (self.out / "byod-build-manifest.json").write_text(json.dumps(self.manifest))
        installer.copy_runtime(self.out, self.stage)
        metadata = self.root / "metadata.json"
        metadata.write_text(json.dumps({
            "Endpoint": "https://wus2.codesigning.azure.net",
            "CodeSigningAccountName": "test-account", "CertificateProfileName": "test-profile",
        }))
        self.args = argparse.Namespace(signing_metadata=metadata, signing_publisher="Test Publisher")

    def test_signing_only_changes_staging_and_records_provenance(self):
        original_hash = installer.sha256(self.out / "byod-build-manifest.json")
        def fake_sign(stage, args):
            self.assertEqual(stage, self.stage)
            for name in ("chrome.exe", "chrome.dll", "extra.dll"):
                (stage / name).write_bytes((stage / name).read_bytes() + b"-signed")
        with patch.object(installer, "sign_path", side_effect=fake_sign):
            installer.sign_staged_runtime(self.stage, self.manifest, self.args)
        packaged = json.loads((self.stage / "byod-build-manifest.json").read_text())
        self.assertEqual(packaged["build_files"], self.manifest["files"])
        self.assertEqual(packaged["signing"]["source_manifest_sha256"], original_hash)
        self.assertEqual(packaged["source_revision"], "test-revision")
        for name, info in packaged["files"].items():
            self.assertEqual(info["sha256"], installer.sha256(self.stage / name))
        self.assertIn("extra.dll", packaged["files"])
        self.assertEqual(installer.verify_build(self.out), self.manifest)
        self.assertEqual(installer.sha256(self.out / "byod-build-manifest.json"), original_hash)

    def test_source_license_is_required_and_copied(self):
        with self.assertRaisesRegex(RuntimeError, "missing Chromium license"):
            installer.copy_license_notices(self.root, self.stage)
        (self.root / "LICENSE").write_text("Chromium test license")
        installer.copy_license_notices(self.root, self.stage)
        self.assertEqual((self.stage / "LICENSE.chromium.txt").read_text(),
                         "Chromium test license")

    def test_runtime_dependencies_exclude_stale_outputs(self):
        stale = self.out / "MEIPreload"
        stale.mkdir()
        (stale / "old.pb").write_bytes(b"old component")
        locale = self.out / "locales/en-US.pak"
        locale.parent.mkdir()
        locale.write_bytes(b"new locale")
        (locale.parent / "old-locale.pak").write_bytes(b"stale locale")
        deps = self.root / "runtime.txt"
        deps.write_text("\n".join(installer.REQUIRED_FILES) + "\nlocales/en-US.pak\n")
        stage = self.root / "filtered-stage"
        installer.copy_runtime(self.out, stage, deps)
        self.assertTrue((stage / "chrome.exe").is_file())
        self.assertEqual((stage / "locales/en-US.pak").read_bytes(), b"new locale")
        self.assertFalse((stage / "extra.dll").exists())
        self.assertFalse((stage / "MEIPreload").exists())
        self.assertFalse((stage / "locales/old-locale.pak").exists())

    def test_runtime_dependencies_require_all_core_files(self):
        deps = self.root / "runtime.txt"
        deps.write_text("chrome.exe\n")
        with self.assertRaisesRegex(RuntimeError, "omit required"):
            installer.declared_runtime_files(self.out, deps)

    def test_runtime_dependencies_reject_path_escape(self):
        deps = self.root / "runtime.txt"
        for unsafe in ("../chrome.dll", "C:/other/chrome.dll", "/chrome.dll"):
            deps.write_text("\n".join(installer.REQUIRED_FILES) + "\n" + unsafe)
            with self.assertRaisesRegex(RuntimeError, "unsafe runtime"):
                installer.declared_runtime_files(self.out, deps)

    def test_runtime_dependencies_missing_file_fails_closed(self):
        deps = self.root / "runtime.txt"
        deps.write_text("\n".join(installer.REQUIRED_FILES) + "\nmissing.dll\n")
        with self.assertRaisesRegex(RuntimeError, "missing declared"):
            installer.declared_runtime_files(self.out, deps)

    def test_service_patch_license_is_shipped(self):
        (self.root / "LICENSE").write_text("Chromium test license")
        marker = self.root / "net/traffic_annotation/byod_google_services.h"
        marker.parent.mkdir(parents=True)
        marker.write_text("// service guard")
        installer.copy_license_notices(self.root, self.stage)
        self.assertIn("BSD 3-Clause", (self.stage / "LICENSE.ungoogled-chromium.txt").read_text())

    def test_modified_staging_is_rejected_before_signing(self):
        (self.stage / "chrome.dll").write_bytes(b"wrong build")
        with patch.object(installer, "sign_path") as sign:
            with self.assertRaisesRegex(RuntimeError, "changed before signing"):
                installer.sign_staged_runtime(self.stage, self.manifest, self.args)
            sign.assert_not_called()

    def test_signing_failure_does_not_rewrite_manifest(self):
        before = (self.stage / "byod-build-manifest.json").read_bytes()
        with patch.object(installer, "sign_path", side_effect=subprocess.CalledProcessError(1, "sign")):
            with self.assertRaises(subprocess.CalledProcessError):
                installer.sign_staged_runtime(self.stage, self.manifest, self.args)
        self.assertEqual((self.stage / "byod-build-manifest.json").read_bytes(), before)
        installer.verify_build(self.out)

    def test_partial_signing_options_are_rejected(self):
        with patch("sys.argv", ["installer", "--signing-metadata", "metadata.json"]):
            with self.assertRaises(SystemExit) as error:
                installer.parse_args()
            self.assertEqual(error.exception.code, 2)

    def test_unsigned_packaging_remains_default(self):
        with patch("sys.argv", ["installer"]):
            self.assertIsNone(installer.parse_args().signing_metadata)


if __name__ == "__main__":
    unittest.main()
