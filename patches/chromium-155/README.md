# Chromium 155 BYOD source baseline

This is a reproducible source checkpoint. The native Windows 155 build now
passes; runtime and signed-package validation are tracked separately below.

Upstream: Chromium **155.0.8059.40**, revision
`cfaadc5a132d78e1828635aa8405a499f3e14864`.

Apply, in order, to that clean upstream revision:

1. `1300-byod-baseline.patch` in this directory. It replaces the older BYOD
   patch stack, including UA/API-key infobar, XHTTP and page audit changes;
   **do not also apply the old 1300–1303 patches**.
2. `../1304-optional-audit-bootstrap.patch`.
3. `1305-browser-command-interface.patch` (Chromium 155 Connect command fix).
4. `1306-audit-tab-close.patch` (distinguish fast tab close from renderer crash).
5. `../google-services/1400-service-removal.patch`.

The baseline is the exact diff from the above upstream to the Windows rebased
checkpoint `8e5a875bc868009f6ddd4268d26b0c11e3734592`: 54 files, 3,111 insertions
and 101 deletions. SHA-256 of the baseline patch:
`ca695b9a31c80ada6a21582aee4ec7689b85d8acdd0eb799d923c757ae187b8a`.

An isolated source-only export of the 38 existing upstream files was used to
check/apply the baseline and check the subsequent optional-audit patch.
The remaining Google-removal baseline files were supplied from the saved
pre-removal source export. Replaying all three patches succeeded, and SHA-256
comparison of all **252 affected files** matched the current Windows source
exactly (2026-10-07 20:27 CST). The authoritative comparison manifest is
`C:\src\chromium\byod-upstream-backup-20261007\repro155-current.json`.
The later GCM compile fixes are incorporated in the Google-removal patch. The
updated source hash for `gcm_client_impl.cc` is:
`0167a0bef5fc1af48023ae874c170954fb2620e3a67c3866ef4f44cf1add4666`, verified
against Windows and the local replay tree at 22:17 CST.
The subsequent domain-reliability uploader compile fix is also incorporated;
its hash is `b4dd923f771d1600c5de9b148cb4802b5b340ebf625f7239e078f3de6faf0063`,
verified against Windows at 22:23 CST. A third fix removes an unused Lens
feature parameter; `components/lens/lens_features.cc` has verified hash
`2f025c42e66ab7ee7fe132954d05adc37afe429127b40d4774c803fa7d788fa4`
(22:33 CST). A fourth fix removes unused model-quality upload callbacks;
`components/optimization_guide/core/model_quality/model_quality_logs_uploader_service.cc`
has verified hash
`aca5325f4424bd79c0565de6f6dabc1f26463797a041e74f51d149dc8ce58f2e`
(22:45 CST). A fifth fix removes Translate's unused retry field and initializer;
`components/translate/core/browser/translate_url_fetcher.cc` has verified hash
`3e0e3ebc9e60bcda61cf5f9f3a65aa4e02c7fa1376e2ae1d0eb1f579a349ebc5`, and its
header has verified hash
`7183f24e5be12c7d56b18336952bc5122e7d2f7ce1fccc7df1dd21dcfe9d0516`.
Those were the six changed files since the initial 252-file comparison, and all
are incorporated in the Google-removal patch. Reverse-apply validation passes
on the complete source-only replay tree after these fixes.
This is source reproduction evidence, not a substitute for compilation, tests
or a DEPS sync. Update the patch/evidence if subsequent build fixes change it.

On 2026-10-08 the collected compile repairs were applied after Ninja exited.
The updated Google-removal patch and separate `1305` one-line command fix pass
reverse-apply checks. A fresh export of **254 affected files** matches the full
local replay tree byte-for-byte (SHA-256); there are no missing files or hash
mismatches. Evidence: `repro155-after-batch-20261008.json` in the same Windows
backup directory. `1305` changes only `OpenGripsLogin`'s parameter to
`BrowserWindowInterface*`, retaining current-tab navigation. Its SHA-256 is
`4582f425b77c97cd850b0cc72cb846b45183b9cf98e0681f7b32330cb93476e4`.
Compilation and runtime/package validation remain incomplete.

The subsequent DLL-link repair updates four service-source files without a
GN change. The expanded **257-file** replay matches the fresh Windows
`repro155-after-linkfix-20261008.json` export with zero mismatches. Its complete
incremental dry run contains only 7 actions; build success remains unproven.

That 7-action build completed successfully at 01:58 CST on 2026-10-08. First
startup exposed an inconsistent Safe Browsing sync preference registration.
The one-file local-only preference repair completed in 5 incremental actions
at 02:15 CST, with a verified canonical manifest and no remaining Ninja work.
The Google-removal patch includes this repair. Full source comparison now
matches **258 files** against `repro155-after-prefs-20261008.json`; reverse
checks pass. Isolated startup, UA, NTP, Connect-button navigation, removed
Google settings/deep links and Grips bridge tests pass. This is not yet proof
of all transport/audit/package gates or broad network-service coverage.

`1306` defers normal renderer-loss reporting by one task using a weak pointer,
so synchronous tab destruction can report `tab_closed` first. Actual crashes
are reported immediately and must not be mistaken for visibility changes.
It does not weaken the audit test to accept the incorrect reason. SHA-256:
`7b5c7826acb646cbc86eea927bb77e80daf7d54f1e3db44dccd4076aaecc7ae4`.
The first 4-action incremental build and canonical manifest verification pass;
`repro155-after-audit-20261008.json` matches that intermediate 258-file replay.
The final crash/visibility refinement also built successfully (4 actions),
verified its manifest and has no remaining Ninja work. Its 258-file replay
matches `repro155-after-audit2-20261008.json`. Final audit runtime validation
was initially delayed by fixture idleness and Windows SSH failures. After SSH
recovered, the fresh 08:43 CST test passed all 22 audit events, including both
actual renderer crash and tab close. The canonical manifest re-verifies and
Ninja reports no work. The signed test installer and independent extraction /
space-containing shortcut startup passed at 08:48 CST. Exact artifact hash and
remaining broad-service/network coverage limits are in the tracking document.

The five patches are already present in the current Windows working tree.
Do not reapply them to that tree or run any preparation script while its
existing Ninja process is active. Build settings, source target and validation
state are tracked in `../../docs/chromium-upstream-google-free.md`.
