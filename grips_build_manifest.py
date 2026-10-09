#!/usr/bin/env python3
"""Write or verify the integrity record for a Grips Chromium output."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from datetime import datetime, timezone
from pathlib import Path
import subprocess
import sys


SCHEMA = 1
REQUIRED_FILES = (
    "chrome.exe",
    "chrome.dll",
    "resources.pak",
    "icudtl.dat",
    "v8_context_snapshot.bin",
)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def source_fingerprint(root: Path) -> str | None:
    """Hash tracked source changes, including staged and unstaged changes."""
    try:
        result = subprocess.run(
            [shutil.which("git") or "git", "-C", str(root), "diff", "--binary", "HEAD", "--"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return hashlib.sha256(result.stdout).hexdigest()


def git_revision(root: Path) -> str | None:
    try:
        result = subprocess.run(
            [shutil.which("git") or "git", "-C", str(root), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return result.stdout.strip() or None


def collect(root: Path, out: Path) -> dict:
    files = {}
    missing = []
    for name in REQUIRED_FILES:
        path = out / name
        if not path.is_file():
            missing.append(name)
            continue
        stat = path.stat()
        files[name] = {
            "sha256": digest(path),
            "size": stat.st_size,
            "mtime_ns": stat.st_mtime_ns,
        }
    args = out / "args.gn"
    if not args.is_file():
        missing.append("args.gn")
    if missing:
        raise RuntimeError("missing build inputs: " + ", ".join(missing))

    revision = git_revision(root)
    fingerprint = source_fingerprint(root)
    if not revision or not fingerprint:
        raise RuntimeError("cannot determine Chromium source revision and fingerprint")

    return {
        "schema": SCHEMA,
        "root": str(root.resolve()),
        "out": str(out.resolve()),
        "target": "chrome",
        "source_revision": revision,
        "source_fingerprint": fingerprint,
        "args_sha256": digest(args),
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "files": files,
    }


def write_manifest(root: Path, out: Path) -> None:
    manifest = out / "byod-build-manifest.json"
    payload = collect(root, out)
    temporary = manifest.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    temporary.replace(manifest)
    print(f"Wrote {manifest}")


def verify_manifest(root: Path, out: Path) -> None:
    manifest = out / "byod-build-manifest.json"
    if not manifest.is_file():
        raise RuntimeError(f"{manifest} is missing; run the canonical build first")
    try:
        payload = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"cannot read {manifest}: {exc}") from exc

    if payload.get("schema") != SCHEMA:
        raise RuntimeError("unsupported build manifest schema")
    if Path(payload.get("root", "")).resolve() != root.resolve():
        raise RuntimeError("manifest belongs to a different Chromium checkout")
    if Path(payload.get("out", "")).resolve() != out.resolve():
        raise RuntimeError("manifest belongs to a different output directory")

    expected_revision = payload.get("source_revision")
    if not expected_revision or git_revision(root) != expected_revision:
        raise RuntimeError("Chromium source revision does not match the build manifest")

    expected_source = payload.get("source_fingerprint")
    actual_source = source_fingerprint(root)
    if not expected_source or actual_source != expected_source:
        raise RuntimeError("Chromium source changes do not match the built artifacts")

    args = out / "args.gn"
    if not args.is_file() or digest(args) != payload.get("args_sha256"):
        raise RuntimeError("args.gn does not match the build manifest")

    failures = []
    recorded = payload.get("files", {})
    for name in REQUIRED_FILES:
        path = out / name
        expected = recorded.get(name, {}).get("sha256")
        if not path.is_file():
            failures.append(f"{name}: missing")
        elif not expected:
            failures.append(f"{name}: absent from manifest")
        elif digest(path) != expected:
            failures.append(f"{name}: SHA-256 mismatch")
    if failures:
        raise RuntimeError("; ".join(failures))
    print(f"Verified Grips build manifest: {manifest}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("operation", choices=("write", "verify"))
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.operation == "write":
            write_manifest(args.root, args.out)
        else:
            verify_manifest(args.root, args.out)
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
