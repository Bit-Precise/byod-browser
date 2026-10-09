"""Export selected source paths for offline patch porting, without mutations."""

import argparse
import hashlib
import json
import pathlib
import re
import tarfile


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument("--paths", type=pathlib.Path, required=True)
    parser.add_argument("--patches", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    paths = set()
    for line in args.paths.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            paths.add(line)
    if args.patches:
        for patch in args.patches.rglob("*.patch"):
            paths.update(re.findall(r"^--- a/(\S+)", patch.read_text(encoding="utf-8"), re.M))
    records, missing = {}, []
    with tarfile.open(args.output, "x") as archive:
        for name in sorted(paths):
            relative = pathlib.PurePosixPath(name)
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError(f"Unsafe source path: {name}")
            source = args.root / relative
            if not source.is_file():
                missing.append(name)
                continue
            records[name] = hashlib.sha256(source.read_bytes()).hexdigest()
            archive.add(source, arcname=name, recursive=False)
    report = {"root": str(args.root), "sha256": records, "missing": missing}
    args.output.with_suffix(".json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"exported": len(records), "missing": missing}))


if __name__ == "__main__":
    main()
