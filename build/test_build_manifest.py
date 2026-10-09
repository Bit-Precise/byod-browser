"""Build provenance must not accept another checkout or an unreadable source."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "manifest", Path(__file__).resolve().parents[1] / "grips_build_manifest.py")
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix="byod-manifest-test-")
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        self.out = self.root / "out"
        self.out.mkdir()
        for name in (*manifest.REQUIRED_FILES, "args.gn"):
            (self.out / name).write_text(name)

    def write(self):
        with patch.object(manifest, "git_revision", return_value="revision-a"), \
             patch.object(manifest, "source_fingerprint", return_value="fingerprint-a"):
            manifest.write_manifest(self.root, self.out)

    def test_source_revision_is_checked(self):
        self.write()
        with patch.object(manifest, "git_revision", return_value="revision-b"), \
             self.assertRaisesRegex(RuntimeError, "source revision"):
            manifest.verify_manifest(self.root, self.out)

    def test_missing_git_metadata_is_rejected(self):
        with patch.object(manifest, "git_revision", return_value=None), \
             patch.object(manifest, "source_fingerprint", return_value=None), \
             self.assertRaisesRegex(RuntimeError, "cannot determine"):
            manifest.write_manifest(self.root, self.out)

    def test_missing_fingerprint_is_rejected(self):
        self.write()
        path = self.out / "byod-build-manifest.json"
        data = json.loads(path.read_text())
        data["source_fingerprint"] = None
        path.write_text(json.dumps(data))
        with patch.object(manifest, "git_revision", return_value="revision-a"), \
             self.assertRaisesRegex(RuntimeError, "source changes"):
            manifest.verify_manifest(self.root, self.out)

    def test_matching_source_and_binaries_are_accepted(self):
        self.write()
        with patch.object(manifest, "git_revision", return_value="revision-a"), \
             patch.object(manifest, "source_fingerprint", return_value="fingerprint-a"):
            manifest.verify_manifest(self.root, self.out)


if __name__ == "__main__":
    unittest.main()
