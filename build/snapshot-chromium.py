#!/usr/bin/env python3
"""Capture dirty Chromium source and incremental-build metadata before rebase.

Does not stage, reset, delete, or alter any Chromium files. The destination
must not exist. Includes untracked files and separate staged/unstaged diffs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
from datetime import datetime, timezone


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--destination", required=True, type=Path)
    parser.add_argument("--out", default="out/Grips")
    args = parser.parse_args()
    root = args.root.resolve()
    destination = args.destination.resolve()
    if destination == root or root in destination.parents:
        raise SystemExit("Snapshot must live outside the Chromium repository")

    def git(*command):
        return subprocess.check_output(["git", "-C", str(root), *command])

    status_before = git("status", "--porcelain=v1", "-z")
    changed = git("diff", "--name-only", "HEAD", "-z")
    untracked = git("ls-files", "--others", "--exclude-standard", "-z")
    names = sorted(set(n.decode("utf-8") for n in (changed + untracked).split(b"\0") if n))
    for name in names:
        path = root / name
        if not path.resolve().is_relative_to(root) or path.is_symlink():
            raise SystemExit(f"Unsafe snapshot entry: {name}")
        if path.exists() and not path.is_file():
            raise SystemExit(f"Expected a file: {name}")
    total = sum((root / n).stat().st_size for n in names if (root / n).is_file())
    if total > 4 * 1024**3:
        raise SystemExit(f"Unexpectedly large source snapshot: {total} bytes")
    destination.mkdir(parents=True, exist_ok=False)
    info = {"created_utc": datetime.now(timezone.utc).isoformat(),
            "root": str(root), "head": git("rev-parse", "HEAD").decode().strip(),
            "branch": git("branch", "--show-current").decode().strip(),
            "source_files": {}, "build_metadata": {}}
    for filename, command in {
        "status.z": ("status", "--porcelain=v1", "-z"),
        "all-tracked.patch": ("diff", "--binary", "HEAD"),
        "staged.patch": ("diff", "--cached", "--binary"),
        "unstaged.patch": ("diff", "--binary"),
    }.items():
        (destination / filename).write_bytes(git(*command))
    with tarfile.open(destination / "source-files.tar", "x") as archive:
        for name in names:
            path = root / name
            if not path.exists():
                info["source_files"][name] = {"deleted": True}
                continue
            digest = sha256(path)
            archive.add(path, arcname=name, recursive=False)
            if sha256(path) != digest:
                raise SystemExit(f"Source changed during snapshot: {name}")
            info["source_files"][name] = {"sha256": digest, "bytes": path.stat().st_size}
    for name in ("args.gn", ".ninja_log", ".ninja_deps", "toolchain.ninja", "byod-build-manifest.json"):
        path = root / args.out / name
        if not path.is_file():
            continue
        output = destination / ("build-" + name.lstrip("."))
        before = sha256(path)
        shutil.copy2(path, output)
        if sha256(output) != before or sha256(path) != before:
            raise SystemExit(f"Build metadata changed during snapshot: {name}")
        info["build_metadata"][name] = {"sha256": before, "bytes": path.stat().st_size}
    if git("status", "--porcelain=v1", "-z") != status_before:
        raise SystemExit("Working tree changed during snapshot")
    with tarfile.open(destination / "source-files.tar", "r") as archive:
        for member in archive.getmembers():
            digest = hashlib.sha256()
            with archive.extractfile(member) as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            if digest.hexdigest() != info["source_files"][member.name]["sha256"]:
                raise SystemExit(f"Archive verification failed: {member.name}")
    info["archive_sha256"] = sha256(destination / "source-files.tar")
    (destination / "snapshot.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"destination": str(destination), "head": info["head"],
                      "source_files": len(names), "source_bytes": total,
                      "archive_sha256": info["archive_sha256"]}))


if __name__ == "__main__":
    main()
