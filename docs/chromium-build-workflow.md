# BYOD Chromium Windows build workflow

This project uses the remote Windows host `mryao@11.0.16.250` with Chromium
at `C:\src\chromium\src` and the `out\Grips` output directory. Chromium
commands must run through `cmd.exe`; do not use PowerShell.

## Guardrails

1. Keep `out\Grips\args.gn` stable for the lifetime of a build. Do not switch
   `use_lld`, component/debug mode, or toolchain in the same output directory.
2. Before building, run `ninja -n` for the exact target. If a small resource
   change reports thousands of actions, stop and inspect `.ninja_log` and
   `.ninja_deps`; do not proceed blindly.
3. Never run `ninja -t clean chrome.dll` and never delete core outputs to force
   a rebuild. Use the exact target and let Ninja decide its dependencies.
4. Do not copy `chrome.dll`, V8 snapshots, `chrome.exe`, or pak files from an
   installed Chromium into `out\Grips`. Every binary must come from the same
   output directory and build configuration.
5. Resource-only changes should build and verify `resources.pak` without
   rebuilding `chrome.dll`. C++ changes require `ninja -C out\Grips chrome.dll`.
6. Back up current `.ninja_log` and `.ninja_deps` before any `gn gen`; never
   restore an older backup over a newer log.
7. Before interactive testing, verify that `chrome.exe`, `chrome.dll`,
   `resources.pak`, `icudtl.dat`, and `v8_context_snapshot.bin` all exist in
   `out\Grips`. The canonical build also writes
   `out\Grips\byod-build-manifest.json`; the launcher must verify its SHA-256
   entries before starting. Use a fresh profile and close all Chromium
   processes first.
8. If startup crashes, inspect the newest Crashpad dump before changing build
   inputs. Do not mix replacement binaries while diagnosing it.
9. Never copy a DLL, EXE, pak, or V8 snapshot from an installed Chromium into
   `out\Grips`. A manifest mismatch is a build-integrity failure, not a reason
   to delete the output directory. Re-run the exact incremental Ninja target.
10. The destructive full-clean script is opt-in only (`ALLOW_FULL_CLEAN=YES`).

## Standard commands

```cmd
cd /d C:\src\chromium\src
third_party\ninja\ninja.exe -C out\Grips -n resources.pak
third_party\ninja\ninja.exe -C out\Grips resources.pak
third_party\ninja\ninja.exe -C out\Grips -n chrome.dll
third_party\ninja\ninja.exe -C out\Grips chrome.dll
```

The canonical Windows build is `run-full-grips-build.cmd`. It invalidates the
old manifest before Ninja starts and writes a new one only after `chrome`
returns exit code zero. `start-byod-exam-test.bat` calls
`verify-grips-build.cmd` before launching, so an old or mixed output cannot be
mistaken for the current build.

The test BAT must launch `out\Grips\chrome.exe` with a fresh or explicitly
identified profile. Do not test an installer copy when validating an
`out\Grips` build.
