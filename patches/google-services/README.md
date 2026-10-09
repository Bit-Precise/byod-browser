# Google service removal: reviewed reference set

This directory is for the Chromium 155 BYOD service-removal patch series.
`1400-service-removal.patch` is applied on the Windows 155 checkout and builds
successfully. Startup/UI, native H1/H2, final page audit, signatures and signed
shortcut tests pass. A 155 test installer is in the Windows Downloads directory;
see the tracking document for its hash and scope. This is not a claim that all
Google-service network paths were exercised in runtime tests.

Reference: ungoogled-software/ungoogled-chromium commit
`3e46b13825f808f0886e484d44532372655e5fe4` (Chromium 154.0.8037.97).
Port against the verified 155.0.8059.40 source, retaining upstream copyright
and license notices for any reused hunks. Preserve Connect/Grips additions.

The reference is useful for Safe Browsing build guards, request entry points,
Google account clients, cloud messaging, and fail-safe domain substitution.
Do not import its entire build preset or extra UI/privacy patch set:

- Keep local Crashpad dump collection; remove only upload plumbing/endpoints.
- Keep HTTPS validation, HSTS, sandboxing and Windows download quarantine.
- Keep ordinary user-requested websites, including Google-hosted websites,
  reachable unless an exam policy disallows them.
- Do not replace Microsoft/Mozilla/Facebook endpoints just because the
  reference's domain regex file includes them.
- Do not disable standard WebRTC/mDNS, local password storage, browser history,
  downloads, or third-party cookies as a side effect of removing Google APIs.
- Do not add optional UI flags unrelated to this task or downgrade to the
  older Chromium release just to apply the reference patch set unchanged.
- Review asynchronous stubs so service callers receive failure/completion
  callbacks rather than waiting forever for a removed request.

The patch additionally disables Sync/DICE activation, metrics/crash upload,
component updates, Translate, Glic/optimization remote services, Lens, Google
search defaults/NTP, online spellcheck and network geolocation. Google settings
views are removed and old deep links return to the local/Connect profile page.
The page-audit optional-bootstrap compatibility fix is a separate BYOD change.

`build/generate-google-service-guard.py` supplements entry-point removal with a
URLRequest gate keyed by upstream traffic annotations. It does not deny normal
web content based on its hostname. Shared update/suggestion/image clients are
denied only for Google endpoints, including Google's regional domains. The
reviewable `google-services-manifest.json` retains the annotation corpus from
before removals; use `--from-manifest` to regenerate after patching sources.

Keep `LICENSE.ungoogled-chromium` in the packaging tool bundle. The installer
ships this notice and Chromium's license. This does not remove Blink/V8 or all
code authored by Google; the scope is built-in online service integrations.

The broad completion/verification checklist is in
[`docs/chromium-upstream-google-free.md`](../../docs/chromium-upstream-google-free.md).
