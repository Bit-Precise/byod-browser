# Chromium upstream update and Google-services removal

## Requested deliverable

Update Chromium from upstream, remove Google service integrations, and deliver
a newly built and signed Windows installer in the test machine's Downloads.
Existing BYOD / Connect / OIDC / fullscreen / XHTTP / page-audit work must be
preserved. This document tracks work; unchecked items are not completion claims.

## Baselines and target (2026-10-07)

- Authoritative Windows checkout: `C:\src\chromium\src`, output `out\Grips`.
- Previous upstream commit: `c2d350cd776d4024caee095b6cee010613bf02c9`,
  dated 2026-07-12; actual version `152.0.7947.0`.
- `build/chromium-version.env` records the verified 155 source pin, not a
  completed build or installer.
- Chromium Dash reports Windows Stable **155.0.8059.40**, Chromium hash
  `cfaadc5a132d78e1828635aa8405a499f3e14864` (release 2026-10-06).
  This is the update target, not the moving main branch.
- Maintained ungoogled-chromium reference currently targets 154.0.8037.97,
  tag `154.0.8037.97-1`, commit `37085e47cf580c815a30402917d350ce97399ded`.
  Its service-removal mechanisms require review/adaptation for 155; blindly
  applying the entire fork or pretending 154 patches prove 155 coverage is
  not acceptable. Unrelated UI changes/binary pruning are not this task.

Sources: Chromium Dash `fetch_releases?channel=Stable&platform=Windows&num=1`,
official Chromium git, and ungoogled-software/ungoogled-chromium.

## Scope / required verification

- [x] Verified, recoverable snapshot of all existing Chromium edits (including
  staged, unstaged and untracked files), build arguments, Ninja logs and manifest.
- [x] Target upstream revision checked out with existing BYOD changes preserved;
  DEPS synchronized and hooks/toolchain aligned to that revision.
- [ ] Google service integrations removed/disabled at their build/runtime entry
  points: GAIA/DICE/Sync, online Safe Browsing, Google Translate, suggestions,
  account/password cloud services, variations/metrics/crash uploads,
  optimization/model services, component updates/Web Store, Google geolocation,
  Google cloud messaging/push and other built-in Google endpoints found during
  review. No API-key prompt or Google account UI remains.
- [ ] Built-in Google endpoint fallback blocked without blocking user-requested
  ordinary websites or their resources. No substitution of a launcher-only
  flag list for service removal.
- [ ] Blink/V8, source certificate verification and standard web capabilities
  remain intact. Connect login is still a normal website/OIDC flow, not GAIA.
- [x] Current source/build pin recorded reproducibly with reviewed patches and
  applicable third-party attribution.
- [x] Build scope inspected with Ninja dry run; successful canonical build and
  integrity manifest. No cleaning of the output directory or mixed binaries.
- [ ] Source/build inspection plus runtime network tests cover the broad service
  removal claim, not just absence of idle traffic or the API-key infobar.
- [x] Windows regression tests: startup/shortcut, UA marker, Connect entry,
  XHTTP routing and TLS, page-only audit, native leave/expiry/fullscreen behavior.
- [x] Signed installer and inner BYOD binaries verified with timestamps,
  publisher and hashes; independent payload extraction/startup with isolated
  user data. The real installed browser/profile was not overwritten.
- [x] New installer and checksum in `C:\Users\mryao\Downloads`, exact version,
  path/hash and limitations reported to the user.

Disabling Google Safe Browsing removes its malicious-site/download checks;
this must not disable ordinary HTTPS certificate validation. Browser engine
and third-party libraries developed by Google are not themselves online
Google services and are not to be removed indiscriminately.

## Build policy

This is an intentional three-milestone upstream update, not a one-line resource
change. Updated DEPS/toolchain and service build flags can legitimately cause
many recompilations. Inspect and explain the dependency scope; preserve the
existing output and do not change unrelated build modes or clean caches to
force work. Rebase/source changes invalidate the old manifest until a verified
new build completes. Original installers and installed profiles remain untouched.

## Progress / recovery

- **Test installer delivered, 2026-10-08 08:48 CST.**
  `C:\Users\mryao\Downloads\BYOD-Browser-Setup-155.0.8059.40-test-signed-20261008.exe`
  and adjacent `.exe.sha256`, **263,003,616 bytes**. SHA-256:
  `67325eca418fde92c963ff6a54d8c591aaabf55e0d737f121a3bb909001bb7b2`.
  Azure Artifact Signing publisher: **Bit Precise, LLC**, SHA256/RFC3161.
  IExpress failed on this Windows image; the existing managed self-extractor
  fallback produced the signed package. Verification independently extracted
  the embedded ZIP, checked **24 manifest hashes**, **17 own timestamped
  signatures** and **4 preserved vendor signatures**, and confirmed the
  canonical output/manifest were unchanged. Evidence:
  `C:\src\chromium\BYOD Signed Test 155-20261008\verification.json`.
- The signed runtime was launched through an isolated `.lnk` with spaces in
  both executable and profile paths, without disabling the sandbox or GPU.
  `build/smoke-signed-shortcut.mjs` passed: exactly one intended tab, working
  local NTP, version 155.0.8059.40 and matching native/JavaScript UA with
  `BYODBrowser/155.0.8059.40`. The browser was closed normally. No desktop
  shortcut, installed browser, real profile, production service or system
  certificate store was modified. This is a **test build**, not a claim of
  comprehensive service-network coverage or full live OIDC/exam acceptance.
  The unchecked broad-service items above retain those verification limits.
  Final independent Downloads hash/signature checks pass. All 13 exact
  `BYOD155-*` on-demand test tasks and the owned SSH forwarding are removed;
  XML definitions, logs and isolated profiles are retained for recovery.
- 2026-10-08 08:43 CST: Windows SSH is restored. The canonical build manifest
  verifies again, and Ninja reports no work. A fresh native fixture session
  passed the complete **22-event page-audit test**, including the new
  deliberate renderer crash (`renderer_gone`) and real tab close (`tab_closed`)
  checks, expiry, tunnel/direct/redirect/cache/same-document/failure routes,
  subresource and normal-browsing exclusion, and URL redaction. The controlled
  renderer crash/dump in `profile-audit4` is intentional test evidence, not a
  browser-startup failure. Azure signing auth refreshed (expires 10:04 CST).
  Signed-package creation and isolated shortcut tests are now in progress.
- 2026-10-08 runtime follow-up: isolated UI/UA/NTP/Connect-button navigation,
  Google-settings removal/deep-link redirects, Grips bridge and ordinary
  Google-host website navigation/fetch pass. The UI test's old button selector
  and expectation of chrome.send on the login redirect were corrected to
  match the actual Lit page and normal Connect HTTPS transition.
  `ui4-netlog.json` contains 4 observed URLRequest jobs with no violations;
  the fallback annotation gate was **not exercised**, so this capture alone
  does not prove broad service removal.
- H1 and H2 native XHTTP tests pass on 155: each ran 12 concurrent echoes /
  2,949,120 bytes, omitted audit bootstrap, rejected malformed audit fields,
  and cleared all active streams. The extended fixture uses a separate
  untrusted source key: Chromium rejects the inner TLS certificate with
  `ERR_CERT_AUTHORITY_INVALID`, without issuing source HTTP. H2 also passed
  headful native fullscreen enter, F11 lock, and restore. These tests used the
  build preceding only the final one-file audit lifecycle refinement below.
- Page audit passed tunnel/direct navigation, redirect, same-document/cache,
  failure, subresource exclusion, leaving and expiry, but detected fast tab
  shutdown reported as renderer loss. Separate patch `1306-audit-tab-close`
  now defers normal process-exit reporting with a weak pointer. The added
  deliberate renderer-crash test exposed visibility ordering too; actual
  crashes are now reported immediately, and background handling ignores
  crashed contents. Both one-file rebuilds contained **4 actions**, passed,
  and verified their manifests. Final dry run reports no work. Full 258-file
  source replay matches `repro155-after-audit2-20261008.json`.
  The **final audit test remains unverified**: the next attempt reused a
  synthetic session more than 5 minutes after its last stream activity and
  received a disconnected tunnel. `tunnelSessionActive` enforces the idle
  deadline; the ticket itself has an 8-hour TTL, so this must not be described
  as ticket expiry. Retest with a fresh fixture session.
- Around 02:37 CST, new Windows SSH connections began resetting immediately
  after SSH userauth service acceptance. KEX/known host verification succeed;
  repeated read-only commands and an alternate cipher fail before command
  execution. No service restart or Windows system change was attempted.
  Asked the user to restart `sshd` locally. All owned fixture servers were
  stopped through their authenticated endpoint; their SSH forwarding was
  closed. The final audit retest, packaging/signing and isolated signed
  shortcut validation must resume after access is restored. **No 155
  installer has been built/delivered.** Staged package wrapper now targets
  the correctly dated `...155.0.8059.40-test-signed-20261008.exe`; source
  signing auth refreshed successfully, expiry 04:06:11 CST.
- 2026-10-08 01:58 CST: the 7-action final link repair completed with exit 0;
  the canonical manifest was written and verified, and a separate dry run
  reported no work. The first isolated 155 startup then failed at 02:03 with
  `PrefModelAssociator::RegisterPref`: `safebrowsing.enhanced` was still
  registered syncable after removal from the sync database. The log and newest
  Crashpad dump agree; this is not a GPU/SDK failure. Evidence remains under
  `C:\src\chromium\byod-runtime-155-20261008\profile-ui` and `ui.log`.
- The Safe Browsing preference registrations are now local-only (including
  the account-tailored timestamp), with protection defaults off, matching the
  removed services. Sync stays disabled and the invariant is unchanged. Review
  of the patch found just the one removed sync database entry. The fix changes
  one C++ file, no GN/args/toolchain. Dry run showed **5 actions**; the build
  completed at **02:15:37 CST**, exit 0, and manifest verification passed.
  Backup: `byod-runtime-155-20261008\prefs-backup-8281-20669`. Full replay
  comparison `repro155-after-prefs-20261008.json` matches all **258** Windows
  source files. All 41 local build/packaging/guard/NetLog tests pass. Startup,
  transport, audit and signed-package tests still gate delivery.
- Snapshot: `C:\src\chromium\byod-upstream-backup-20261007`, 78 changed or
  untracked source files, 4,032,083 bytes; archive SHA-256
  `0dcfcdd9198c93ff2b82cd14d87efd0e2fba67a1b35668dc142d1238613441d0`.
  Separate original index/worktree diffs and verified build metadata are saved.
- A local source-only checkpoint (54 compilation-related files, including the
  previously untracked Grips headers/resources) is retained on
  `byod/snapshot-20261007`, commit
  `c52e3298e1f97151fe40ec53aca68a7eb18762a9`.
- Update work uses separate branch `byod/google-free-155-20261007`. Nothing
  has been pushed. Historical untracked diagnostic files remain untouched.
- Official googlesource fetch timed out during negotiation. Fetch from the
  official `github.com/chromium/chromium` mirror completed. Its 155.0.8059.40
  tag resolves to `cfaadc5a132d78e1828635aa8405a499f3e14864`, matching Chromium
  Dash; `chrome/VERSION` also matches. Version pin now records this source
  target, not a completed build.
- Google removal review is using ungoogled-chromium as a reference; the entire
  preset is not suitable unchanged (it also modifies non-Google services,
  download quarantine, unrelated privacy features and UI). Retain local crash
  dumps and TLS verification; remove crash **upload**, not debugging support.
  Reviewed reference checkout is commit
  `3e46b13825f808f0886e484d44532372655e5fe4` (same 154.0.8037.97 base as the
  release tag above, plus repository CI changes).
- The page-audit server API has not yet been deployed. The new installer must
  not break exam routing when the older control plane omits audit bootstrap
  fields; preserve audit capability when the updated server supplies them.
- Rebase completed at local Windows commit
  `8e5a875bc868009f6ddd4268d26b0c11e3734592`. Connect settings were ported from
  the removed Polymer template to the new Lit template. `gclient sync --nohooks
  --jobs=8` and `gclient runhooks` both completed successfully, with the root
  solution marked `managed: False` to preserve the BYOD branch.
- The first GN attempt identified a missing SDK. Windows SDK
  **10.0.28000.2526** is now installed side-by-side with 26100; its Include/Lib
  directory is `10.0.28000.0`. No old SDK or build output was removed.
- `1400-service-removal.patch` has been applied and extended. GN subsequently
  exposed two unconditional RLZ dependencies; the obsolete installer cleanup
  call and example target dependency were removed/guarded. GN then completed:
  **35,583 targets from 5,004 files**.
- An initial `ninja -n chrome` only reported the generator edge because its
  recorded GN stamp predated the new BUILD files. This is **not** evidence of
  a one-action browser rebuild. Refresh `build.ninja` alone, then inspect
  `dry-run-155-full.log` before running compilers.
- The complete dry run reported **57,933 actions**. The canonical build started
  with `ninja -C out\Grips -j8 chrome` using `build/build-google-free-155.cmd`.
  Remote log: `C:\src\chromium\byod-upstream-backup-20261007\compile-155.log`.
  The old success manifest was moved recoverably into the backup directory;
  a new manifest will only be written after Ninja succeeds. Do not package or
  launch this output while compilation is incomplete.
- Service follow-up: disable Lens entry points, use the local third-party NTP,
  omit bundled Google search/default activation without overwriting saved
  profiles, remove Sync/Google-services settings views and redirect old deep
  links, disable enhanced online spellcheck, and replace Google network
  geolocation with an asynchronous unavailable result (native OS geolocation
  remains). These changes still require native build/runtime verification.
- The annotation safety net preserves the **pre-removal** corpus in its
  manifest, including services whose request code has since been removed.
  Mixed-use service clients use 278 Google service/country domains generated
  from upstream's `google_tld_list.h`, not a global website blocklist.
- Local checks: 12 annotation-generator tests, 11 installer signing/license
  tests, and 4 build-manifest provenance tests pass. C++ guard/search tests are
  added but not yet compiled or run. No 155 installer is available yet.
- Test-package tools are staged at
  `C:\src\chromium\byod-google-free-package-20261007`. Packaging remains gated
  on a successful canonical manifest. `build/package-google-free-155.cmd`
  obtains the current GN `//chrome:chrome runtime_deps` list and copies only
  declared distribution files, excluding old optional DLLs/component data and
  old locales without cleaning the shared build output. Four packaging tests
  cover stale file exclusion, missing core/files and path traversal rejection.
- During the first build, `tcp_connect_job.obj` (native tunnel) and
  `url_request.obj` (service request gate) compiled successfully at actions
  14,077 and 14,135 respectively. This is compile evidence only, not a passed
  transport/runtime test or a completed browser build.
- `patches/1304-optional-audit-bootstrap.patch` preserves the separate native
  bootstrap compatibility change against the rebased BYOD checkpoint. Both
  audit fields may be absent; partial/malformed fields are rejected before
  routing changes. This patch is already present in the compiling Windows
  source and must not be applied there a second time. Runtime verification is
  pending with `smoke-native-xhttp.mjs --without-audit --check-invalid-audit`.
- `build/smoke-google-free-ui.mjs` is prepared for the new isolated browser:
  verify the UA version, retained Connect settings entry, removed Google
  settings views/deep-link redirects and native Grips bridge. Syntax checking
  passed; execution still awaits the new binary. It does not claim full
  background-service/network coverage.
- Runtime network-check preparation: `build/verify-google-service-netlog.py`
  inspects traffic annotation IDs against the preserved service manifest,
  including endpoint-scoped shared clients and redirect jobs. An observed
  guarded job must finish with `ERR_BLOCKED_BY_CLIENT` (-20), without observed
  send/read events; a DNS error or incomplete job is not accepted as proof.
  Fourteen synthetic parser tests pass. Native capture/verification is pending.
  Use a fresh profile with `--log-net-log=<private-new-file>` and **without**
  `--disable-background-networking`, `--disable-sync`, component-update or
  similar service-suppression flags. Close the browser normally before copying
  the completed NetLog. Raw NetLogs are private; the report omits paths,
  credentials and queries. No-traffic captures do not prove all services off.
  The UI smoke accepts an optional `https://www.google.com/` fixture URL,
  mapped only in this browser to the loopback native fixture with its test SPKI.
  It checks actual navigation plus fetch to that synthetic website, preserving
  normal website access despite the Google hostname. Analyze with
  `python3 build/verify-google-service-netlog.py CAPTURE --require-host www.google.com`.
  The report explicitly distinguishes observed guard activity from a guard
  that was not exercised; this does not replace source or native unit tests.
- The first Ninja run (PID 14208, exec session 14947) exited 1 at 22:09 CST
  after 30,140 attempted / 57,933 actions. The sole diagnostic was
  `gcm_client_impl.cc:403: -Werror,-Wunreachable-code`: the disabled MCS client
  retained its old body after an unconditional return. That unreachable body
  has been removed; Google messaging remains disabled. No warning flags,
  build configuration or cache were changed.
- Dry run after this one-file fix reports **27,794 remaining actions**, with
  the failed GCM object first and no regeneration/full rebuild. The canonical
  build wrapper was resumed at 22:13 CST in the same output directory.
  Latest resource check showed approximately 79 GiB disk and 43 GiB RAM free.
- That retry exposed two unused definitions in the same GCM file:
  `kSendMessageFromValue` and `ToCheckinProtoVersion`. Both belonged to removed
  cloud messaging operations and were deleted, without relaxing warnings.
  Retry 2 exited after 19 actions; its 18 successful actions remain reusable.
  The next dry run reports **27,776 remaining actions**. Retry 3 started at
  22:18 CST, exec session **82191**, using the same canonical wrapper/output.
  Sessions 14947 and 89297 are terminal; do not poll them as live builds.
- GCM compiled successfully at retry-3 action 24; its object is 220,927 bytes,
  timestamp 22:18:58 CST. Retry 3 later stopped at action 335 due to a second
  disabled-service residue: `components/domain_reliability/uploader.cc:93`
  retained unreachable network-upload code. The obsolete upload body and its
  unused MIME constant were removed, preserving the completion callback.
  Only this source file changed; no compiler flags/headers/build args changed.
- The following dry run reported **27,442 remaining actions** (30,491 of the
  original actions completed). Retry 4 was started at 22:23 CST. Session 82191
  is terminal; use the new live session. Remote evidence:
  `domain-uploader-fix.patch`, `dry-run-resume-domain-uploader-155.log`.
- Retry 4 is exec session **76631**, Ninja PID **4524**, started 22:23:49 CST.
  The domain reliability uploader compiled successfully at action 6; its
  object is 31,851 bytes, timestamp 22:24:31 CST. Keep this live build running;
  no new manifest or installer has been produced yet.
- Retry 4 compiled the native XHTTP objects at actions 492
  (`network_service_proxy_delegate.obj`) and 501 (`network_context.obj`).
  It then exited after action 820 because Lens' now-disabled omnibox entry
  left one unused anonymous-namespace feature parameter. That three-line
  definition was removed. Session 76631 is now terminal.
- Dry run after the Lens fix reports **26,623 remaining actions**; retry 5
  was started at 22:34 CST, without cleaning, changing args or disabling
  warnings. Remote repair evidence is `lens-unused-fix.patch` and
  `dry-run-resume-lens-unused-155.log`.
- Retry 5: exec session **94455**, Ninja PID **16824**, started
  22:34:11 CST. At 22:36 CST it reached **245 / 26,623** resumed actions,
  with eight compiler children alive and no new error. Do not start another
  Ninja; sessions 14947, 89297, 82191 and 76631 are terminal.
- Lens compiled successfully at resumed action 13; its object is 163,232
  bytes, timestamp 22:34:52 CST. GCM, domain-reliability and Lens compile
  failures are resolved. Full browser linking/runtime/packaging remain pending.
- Retry 5 exited after 1,754 actions at 22:42 CST: the disabled model-quality
  upload implementation retained an unused `OnURLLoadComplete` helper.
  Removed it and its now-unused upload-status histogram helper in the same
  source file. Dry run reports **24,870 remaining actions**, with the repaired
  object first; retry 6 was started at 22:45 CST. Session 94455 is terminal.
  Remote evidence: `model-quality-unused-fix.patch` and
  `dry-run-resume-model-quality-155.log`. No warning/build flags changed.
- Retry 6: exec session **40678**, Ninja PID **7296**, started
  22:46:05 CST. At 22:48 CST it reached **80 / 24,870** resumed actions,
  with eight compiler children active. Session 94455 is terminal.
  If another compile error stops this run, use Ninja's `-k 0` on the next
  inspected resume to collect independent errors in one pass; keep `-j8`,
  strict warning checks, GN args and the output directory unchanged. Do not
  edit the remote batch wrapper while it is executing.
- Model-quality uploader compiled successfully at retry-6 action 22; its
  object is 73,682 bytes, timestamp 22:46:54 CST. This resolves the fourth
  affected source file's compile failure, not the whole browser build.
- Retry 6 exited 1 at 22:49:03 CST after 361 attempted actions. Translate's
  disabled request implementation retained an unused `max_retry_on_5xx_`
  private field. Removed the field and constructor initializer, preserving
  the constructor API and asynchronous failure callback. The two-file patch
  is saved as `translate-unused-fix.patch`; the successful dry run reports
  **24,512 remaining actions** (`dry-run-resume-translate-155.log`).
- Retry 7 started at 22:56:43 CST: exec session **82425**, Ninja PID **19860**,
  `ninja -C out\Grips -j8 -k 0 chrome`. Independent targets continue after an
  error, but the build still fails unless all required targets succeed.
  No warning flags, GN args, output directory or toolchain were changed.
  At 22:57 CST the run reached 137 / 24,512 actions with zero failed edges.
  Translate compiled successfully: object size 31,404 bytes, timestamp
  22:57:28 CST. Session 40678 is terminal; do not restart or poll it as live.
  The new read-only `build-status-current-155.cmd` in the backup directory
  scans errors since the latest `BUILD_START`, not only the log tail, and
  separately reports live compiler/Ninja processes.
- Retry 7's keep-going pass found four unused definitions in
  `autofill_crowdsourcing_manager.cc`: `kFetchTimeout`,
  `kGoogEncodeResponseIfExecutable`, `GetNetworkTrafficAnnotation`, and
  `GetAPIBodyPayload`. Other targets continue compiling. A one-file cleanup
  is prepared at `/tmp/byod-upstream-xb2VkX/autofill-unused-fix-pending.patch`
  and passes `git apply --check` against the matching local source tree.
  **It is not applied to Windows or the canonical Google patch yet**; wait
  for this live Ninja to exit before applying the collected fixes. The
  pre-removal annotation manifest retains `autofill_query`/`autofill_upload`.
  A later poll reached **2,510 / 24,512** resumed actions with exactly one
  failed edge (the four diagnostics above).
- At 2,742 / 24,512 actions, retry 7 had two failed edges. The new failure is
  `file_system_access_safe_move_helper.h:98`: unused private field
  `has_transient_user_activation_`, whose only consumer was the removed Safe
  Browsing after-write request. Prepared
  `/tmp/byod-upstream-xb2VkX/safe-move-unused-fix-pending.patch`, removing only
  the field and its initializer. Constructor API and Windows quarantine/
  mark-of-the-web handling remain unchanged. This two-file patch passes local
  `git apply --check` but is **not yet applied** to Windows or the canonical
  patch. Wait until Ninja PID 19860 / exec session 82425 exits, then apply all
  collected fixes and inspect the next dry run. Do not launch a second Ninja
  or change active build sources while this keep-going pass runs.
  A later verified poll reached **6,946 / 24,512** attempted actions with the
  same two failed edges; Ninja PID 19860 / exec session 82425 remained live.
  Read-only resource checks showed about 79 GiB free on C: and 39 GiB free
  physical memory; no disk/memory exhaustion is evident. No runtime or
  signing/package verification has started for the 155 output.
- Retry 7 reached 8,300 / 24,512 actions with a third failed edge:
  `v4_update_protocol_manager.cc` still used four `SBER_LEVEL_*` enumerators
  after removing the Safe Browsing preferences include and `using enum`.
  The shared wire-format helper is retained but now prepares a constant
  `ChromeClientInfo::OPT_OUT`, consistent with disabled Google reporting,
  rather than bringing the removed preference dependency back. This one-file
  repair is prepared at
  `/tmp/byod-upstream-xb2VkX/v4-reporting-fix-pending.patch` and passes local
  `git apply --check`. It is not applied to Windows or the canonical patch
  yet. Apply it with the other two pending patches only after this live
  keep-going pass exits; do not restart merely because a poll times out.
  A verified poll reached **9,708 / 24,512** attempted actions, still three
  failed edges. Ninja PID 19860 and exec session 82425 remain live. All three
  pending patches are local only; source/manifest/runtime/signing gates are
  still incomplete for the new binary.
- At 10,062 / 24,512 actions the fourth failed edge was
  `ssl_blocking_page_base.cc`: unused anonymous helper `GetPrefs`, left after
  removing the Enhanced Protection promotion/reporting UI. Prepared
  `/tmp/byod-upstream-xb2VkX/ssl-interstitial-unused-fix-pending.patch`, deleting
  only that helper and its two unused includes. Certificate validation and
  the certificate-error interstitial remain unchanged. The patch passes local
  `git apply --check`; like the other three pending fixes it is not applied to
  Windows or the canonical patch while retry 7 is running.
- At 11,241 / 24,512 actions the fifth failure was
  `extensions/browser/updater/update_service.cc`: the removed Omaha attribute
  action left `should_perform_action_on_omaha_attributes` assigned but unused.
  Prepared `/tmp/byod-upstream-xb2VkX/extension-update-unused-fix-pending.patch`,
  removing that local flag and its assignments while preserving the update
  state switch and `update_found_callback`. The patch passes local apply
  checks, but is not yet applied to Windows or the canonical Google patch.
  A verified poll reached **11,864 / 24,512** attempted actions, five failed
  edges; Ninja PID 19860 and exec session 82425 are still live. The five
  pending fixes cover six files and must be applied after this run exits.
- At 13,911 / 24,512 actions the sixth failure was
  `google_url_loader_throttle.cc`: unused `IsCoveredRequestBoundSessionStatus`
  helper and unused private header field `sends_cookies_`. Prepared
  `/tmp/byod-upstream-xb2VkX/google-throttle-unused-fix-pending.patch`, deleting
  only those two leftovers from the disabled Google request modification path.
  It passes local apply checks and remains unapplied to Windows/canonical patch.
  The previously unmodified header was exported from Windows (empty `git diff`)
  to `google-throttle-original.h`, SHA-256
  `22bdc6b65cb2efc4cd19549455f59114da744eac494295e03c2dc1a12ddbde8f`.
  It was added unchanged to the three isolated source trees so the future
  canonical diff records only the one-line removal, not a new upstream file.
  Scratch baseline heads are now `2c43c8b` (services), `701667d` (remote-state),
  and `129d983` (sourcecheck); no root-project or Windows commit was made.
  Reverse-apply checking of the existing canonical patch still passes.
  The six pending fixes now span eight files; the new header makes the full
  affected-file set 253 once the fixes are applied. Preserve the historical
  252-file comparison rather than claiming it covers this new header.
  A verified poll reached **16,078 / 24,512** attempted actions, six failed
  edges. Ninja PID 19860 / exec session 82425 remain live. No pending patch
  has yet been applied to the compiling source or canonical Google patch.
- At 17,009 / 24,512 actions retry 7 found its seventh failure:
  `chrome/browser/search/search.cc:199` kept the provider-NTP code after the
  unconditional desktop local-NTP return. Prepared
  `/tmp/byod-upstream-xb2VkX/search-ntp-unreachable-fix-pending.patch`: keep
  provider-NTP code and its two private helpers inside the existing Android
  branch, and use only the local third-party NTP on desktop. Incognito/Guest
  handling is unchanged. Local patch-apply checks pass; it is not yet applied
  to Windows or the canonical Google patch. The isolated UI smoke now also
  checks that `chrome://newtab/` loads the local third-party NTP; only JS syntax
  has been checked until the new binary is available.
- The eighth failure is not C++: archiving
  `obj/chrome/browser/safe_browsing/safe_browsing.lib` fails because the disabled
  Windows target has no inputs (`warning: no input files, not writing output
  file`, then `/WX` makes it fatal). Full captured evidence:
  `/tmp/byod-upstream-xb2VkX/compile-155-link-observed.log`, lines 51024-51031.
  Prepared `safe-browsing-empty-target-fix-pending.patch` in the same scratch
  directory, using `group` for this label only on Windows with
  `safe_browsing_mode == 0`; other configurations retain `static_library`.
  Local patch-apply checks pass. GN evaluation and the actual link still await
  application after retry 7 exits. Do not add dummy objects or suppress `/WX`.
  **This pending fix changes BUILD.gn**: back up the current `.ninja_log` and
  `.ninja_deps` before any GN regeneration, retain args/toolchain/output, then
  inspect the complete `ninja -n chrome` scope. If dry run only shows the
  generator edge, refresh `build.ninja` alone and rerun the full dry run before
  compiling. Never restore an older Ninja log over completed work.
  A verified poll reached **18,158 / 24,512** attempted actions, eight failed
  edges. Ninja PID 19860 / exec session 82425 remain live. Eight pending patches
  span ten files (including BUILD.gn); none are applied to Windows/canonical
  source yet. Keep collecting new failures without interrupting this pass.
- At 18,324 / 24,512 actions the ninth failure was
  `extension_safety_check_utils.cc`: unused anonymous helper
  `SafetyCheckShouldShowOffstoreExtension` after removal of its UI branch.
  Prepared `/tmp/byod-upstream-xb2VkX/extension-safety-unused-fix-pending.patch`
  to delete only that helper. Local apply checks pass. It remains unapplied,
  along with the other pending repairs, while Ninja PID 19860 is active.
- At 18,907 / 24,512 actions the tenth failure was
  `https_upgrades_util.cc:95`: `user_has_modified_settings` was no longer used
  after removing its Enhanced Safe Browsing pairing branch in
  `ComputeInterstitialState`. Prepared
  `/tmp/byod-upstream-xb2VkX/https-upgrades-unused-fix-pending.patch`, deleting
  only that variable/comment. The same-named variable in `IsBalancedModeEnabled`
  is still used and is retained, as are HTTPS upgrades and certificate checks.
  Local patch-apply checks pass; Windows/canonical source remains unchanged.
  Latest verified poll: **19,622 / 24,512** attempted actions, ten failed edges.
  Ninja PID 19860 / exec session 82425 remain live. Ten pending patches span
  twelve files (one is BUILD.gn); apply only after the run exits, preserving
  completed outputs and backing up Ninja logs before regenerating GN.
- On 2026-10-08 the local exec handle 82425 became unavailable (`Unknown
  process id`). An independent SSH/CIM/log inspection confirmed Ninja PID
  19860 was **still live**, so no build was restarted. Use
  `build-status-current-155.cmd` in the remote backup directory for subsequent
  monitoring instead of relying on that missing local handle. A later poll
  reached **20,234 / 24,512** attempted actions with the same ten failures.
  All ten pending patches pass a combined `git apply --check` on each of the
  three local source mirrors. They are still unapplied to Windows/canonical
  source. The local scratch directory now also contains
  `repair-batch-155.cmd` and `repair-batch-155-paths.txt` (12 affected files).
  The prepared helper refuses a live Ninja/compiler/linker, snapshots current
  source, Ninja logs/deps and args, applies the batch, runs GN with unchanged
  args, then captures a dry run **without resuming compilation**. Inspect that
  full dry run; a generator-only result still requires refreshing `build.ninja`
  alone and a second complete dry run. The helper has not been executed on
  Windows in mutation mode. The patches and helper have been uploaded to the
  backup directory, and its read-only `--check-only` mode passed on the actual
  Windows source through zero-argument `check-batch-155.cmd` (SSH arguments to
  a CMD script hit a shell-syntax error; the wrapper avoids that quoting issue).
  The planned installer filename in the local packaging wrapper now
  ends in `test-signed-20261008.exe`; it does not yet exist, and the staged
  Windows packaging wrapper must be refreshed before use.
  Follow-up: the dated packaging wrapper and the expanded reproduction path
  list were subsequently uploaded to their existing Windows staging/backup
  directories. Packaging has not been started; the old installer is untouched.
- At **20,469 / 24,512** actions the eleventh failure was
  `chrome/browser/enterprise/signals/context_info_fetcher.cc`: it still called
  the removed device-signals helpers `GetSafeBrowsingProtectionLevel` and
  `GetPasswordProtectionWarningTrigger`. Prepared
  `enterprise-context-fix-pending.patch` to report `NO_SAFE_BROWSING` and
  `std::nullopt` directly, without restoring cloud-service preference access.
  Profile ID, site-isolation, DNS and OS-firewall collection stay unchanged.
  All eleven patches (13 files) pass combined local checks and the read-only
  Windows `check-batch-155.cmd`; none have been applied yet. The batch helper
  and its path list have been extended and uploaded; GN/runtime validation
  remains pending until this build exits.
  This `.cc` was previously unchanged/missing from the isolated mirrors.
  Read-only Windows `git diff HEAD` was empty and its Git blob matched the
  exported file (`d5682e2b0c8ad5a42ad82423d33c2e23eec3deb0`); SHA-256 is
  `06a61b93c918233872d343496cc18414a8ba4d7812ccbe982095f27fd2f285e9`.
  It was recorded unchanged in the scratch baselines only: new heads
  `4c9fa1b` (services), `5e1d728` (remote-state), `7b68019` (sourcecheck).
  The current canonical patch still passes reverse-apply checking.
  `repro155-paths.txt` now includes this source and the throttle header, so the
  next full source comparison must cover **254 files**, not the historical
  252-file snapshot.
- At **21,617 / 24,512** actions, retry 7 reported two additional failed
  objects in download code: `chrome_download_manager_delegate.cc` has an
  unused `IsForceSaveToCloud` helper with download protection disabled, and
  `download_warning_desktop_hats_utils.cc` has an unused
  `warning_first_shown_surface`. Prepared `download-fixes-pending.patch`:
  move the helper inside the same `SAFE_BROWSING_DOWNLOAD_PROTECTION` guard
  as its callers, and remove the unused warning variable plus the orphaned
  anonymous `SerializeWarningActionEvents` helper. Download flow and Windows
  quarantine are not changed by this repair. Both exported Windows source
  files match the local source baseline. The 12 pending patches now cover
  **13 failed edges / 15 files**, including one BUILD.gn. Combined local
  apply checks pass; none are applied to Windows or the canonical patch.
  The two temporary local edits used to generate the diff were restored and
  byte-compared to the Windows exports; the canonical reverse-apply check
  still passes. The batch helper/path list include the new repair. No new
  full-reproduction paths are needed because these two files already belong
  to the 254-file set.
- At **22,116 / 24,512** actions, the fourteenth failed edge was
  `chrome/browser/ui/browser_command_controller.cc`. Its BYOD
  `OpenGripsLogin` helper still took the removed legacy `Browser*`, while
  Chromium 155's command controller passes `BrowserWindowInterface*`.
  Prepared the one-line `browser-command-interface-fix.patch` in scratch;
  the already-ported toolbar helper uses the same modern interface. This
  retains current-tab Connect login behavior. The patch passes checks on the
  full local replay tree and Windows, and is included in the batch helper
  (13 patches / 14 failed edges / 16 files). All remain unapplied.
  **This is a BYOD upstream-port repair, not a Google-removal delta.** When
  applying it after Ninja exits, apply it to Windows and the complete
  `/tmp/byod155-sourcecheck-SdHowD` replay tree, then preserve it separately as
  `patches/chromium-155/1305-browser-command-interface.patch` and add it to the
  documented clean-upstream apply order. Do not feed it to the service-only
  `services` / `services-remote-state` mirrors, which do not contain that
  source, or include it in `1400-service-removal.patch`. The file already
  belongs to the 254-path full reproduction set. The source replay copy was
  byte-compared to the fresh Windows export before checking the patch.
- At **24,093 / 24,512** actions the fifteenth failure was
  `chrome/browser/ui/safety_hub/revoked_permissions_service.cc`: unused
  `notification_display_manager` after removing the cloud-backed notification
  manager. Prepared `revoked-permissions-fix-pending.patch` to remove that
  unused setup and the adjacent orphaned V5-manager setup; retain the display
  manager headers because another method still uses them. Local and Windows
  read-only patch checks pass. The batch helper now has **14 patches / 15
  failed edges / 17 files**, all still unapplied. Latest live poll reached
  **24,281 / 24,512**; Ninja PID 19860 is active. Source reproduction remains
  254 paths. Azure CLI silently refreshed the code-signing token successfully
  (only expiration printed: 2026-10-08 02:32:47 local); this is authentication
  readiness, not proof of signed 155 artifacts.
- Retry 7 **exited with code 1 at 2026-10-08 01:34:33 CST**, after
  **24,347 / 24,512** attempted actions; final inventory has exactly the 15
  known failed edges. Independent process inspection found no remaining
  Ninja/compiler/linker. The full final log is retained locally as
  `compile-155-retry7-final.log` in scratch. The batch helper then applied all
  14 repairs (17 files) on Windows after backing up source, current Ninja
  logs/deps, args and the final log to
  `C:\src\chromium\byod-upstream-backup-20261007\batch-repair-backup-1096-32671`.
  GN succeeded (35,583 targets / 5,004 files) and args remained byte-identical.
  All Google-removal deltas are now applied to both service mirrors and the
  full replay tree and incorporated into `1400-service-removal.patch`;
  the separate BYOD fix is `patches/chromium-155/1305-browser-command-interface.patch`.
  Reverse checks pass. **No pending repair should be applied again.**
  A fresh Windows export (`repro155-after-batch-20261008.json` / `.tar`) covers
  254 files, all of whose SHA-256 values match the full local replay tree.
  The first dry run showed only the stale GN generator edge, so a guarded
  `refresh-batch-gn-155.cmd` is refreshing only `build.ninja` after another
  current-log backup. Inspect its complete `dry-run-after-refresh.log` in the
  batch-backup directory before starting the next compilation. Build/runtime/
  signing and installer-delivery gates are still incomplete.
- The generator refresh completed. `gn desc` confirmed the disabled Windows
  Safe Browsing label is an empty **group**; the initial diagnostic query for
  its `type` property was unsupported, so the read-only query was corrected
  to show the target description. No compile failed during that diagnostic.
  The complete dry run then showed **187 actions: 26 CXX, 156 LIB, 1 RC,
  2 ACTION, 1 DLL link and 1 EXE link**. All 187 actions were reviewed:
  the changed sources/header dependents plus previously blocked archives and
  final binaries, not a full rebuild. Full evidence is
  `batch-repair-backup-1096-32671\dry-run-after-refresh.log` remotely and
  `dry-run-batch-full.log` locally. Latest pre-refresh log backup is
  `batch-repair-backup-1096-32671\gn-refresh-2311-614`.
  After independently confirming no compiler/linker/Ninja remained, the
  canonical `run-google-free-155.cmd` was started again with unchanged
  `-j8 -k 0 chrome`. Source/args/toolchain are frozen for this run. The new
  build has not yet passed, and packaging remains gated on success.
- Retry 8 ran with Ninja PID 14344 / exec session 31008 from 01:46:07 CST.
  All C++ repairs compiled successfully. It reached **185 / 187** and exited
  **1 at 01:47:48** on the final DLL link, with five unresolved symbols in
  four objects: PingManager's removed Safe Browsing experiment helper,
  two SecurityEventRecorderFactory methods retained by SyncServiceFactory,
  the removed generated Safe Browsing preference key, and enterprise
  connectors' affiliation wrapper from an excluded cloud-scanning source.
  The final log is `compile-155-retry8-final.log` locally.
  Applied `link-fixes-155.patch` after confirming the run was terminal:
  remove the experiment call and generated-pref allowlist entry, remove the
  security-event factory dependency (Sync is still hard-disabled), and call
  the same existing `enterprise_util::IsProfileAffiliated` implementation
  directly on desktop without restoring cloud-scanning code. No GN/build
  arguments were changed. Recovery snapshots are in
  `link-repair-backup-4581-32420` under the remote backup directory.
  The updated Google patch is applied to Windows and all three local mirrors
  and passes reverse checks. Two unchanged source baselines were added only
  to the scratch repositories; their heads are now `04f85c7` (services),
  `90222cb` (remote-state), `210d605` (full replay). No root/Windows commit was
  made. The full source set is now **257 files**, all matching the fresh
  Windows `repro155-after-linkfix-20261008.json` export with no missing files.
  Complete dry-run evidence `dry-run-after-link-repair.log` shows **7 actions**
  (4 CXX, DLL link, EXE link, import reorder). The canonical runner was
  resumed after another live-process check, keeping `-j8 -k 0` and all
  outputs/args/toolchain. Runtime and installer gates remain unverified.
- The rebased 54-file BYOD checkpoint is exported as
  `patches/chromium-155/1300-byod-baseline.patch`, with its upstream pin,
  SHA-256 and exact application order documented beside it. Applying this
  baseline and checking the optional-audit follow-up succeeded on an isolated
  export of the corresponding upstream source files. No Windows source or
  active build input was changed by this export/check.
- All three patches were then replayed locally using the saved pre-removal
  service-source baseline. A fresh read-only Windows export reported 252
  affected files; **all 252 SHA-256 hashes match** the replayed local files
  (2026-10-07 20:27 CST). Evidence is saved in the backup directory as
  `repro155-current.json` and `repro155-current.tar`. This is exact current
  source reproduction, not evidence of completed compilation/runtime tests.
- The GCM fix is included in `1400-service-removal.patch`. Its updated source
  hash matches Windows and both local reproduction/porting trees:
  `0167a0bef5fc1af48023ae874c170954fb2620e3a67c3866ef4f44cf1add4666`.
  `repro155-current.json` is the earlier 20:27 snapshot; this and the domain
  reliability uploader below have changed since that comparison. The incremental remote patch and dry-run
  evidence are `gcm-unreachable-fix.patch`, `gcm-unused-fix.patch`,
  `dry-run-resume-gcm-155.log` and `dry-run-resume-gcm-unused-155.log`
  in the backup directory. The revised canonical patch passes reverse-apply
  checks in both local source trees.
- The updated domain reliability uploader also matches Windows/local source:
  `b4dd923f771d1600c5de9b148cb4802b5b340ebf625f7239e078f3de6faf0063`.
- The updated Lens source matches Windows/local source:
  `2f025c42e66ab7ee7fe132954d05adc37afe429127b40d4774c803fa7d788fa4`.
  This is the third changed file since the full 252-file comparison; all
  three compiler fixes are incorporated into the canonical Google patch.
- Model-quality uploader source also matches Windows/local source:
  `aca5325f4424bd79c0565de6f6dabc1f26463797a041e74f51d149dc8ce58f2e`.
  This fourth file's fix is incorporated in the canonical patch and passes
  reverse-apply checks on the local replay tree.
- Translate's source/header also match Windows and both local replay/porting
  trees: `.cc` SHA-256
  `3e0e3ebc9e60bcda61cf5f9f3a65aa4e02c7fa1376e2ae1d0eb1f579a349ebc5`,
  `.h` SHA-256
  `7183f24e5be12c7d56b18336952bc5122e7d2f7ce1fccc7df1dd21dcfe9d0516`.
  Six files now differ from the historical 20:27 comparison manifest. All
  fixes are incorporated in the canonical Google patch; reverse-apply checks
  on the full local replay tree pass. Runtime verification is still pending.
