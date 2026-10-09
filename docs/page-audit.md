# Page-only exam audit

## Scope and privacy

The native observer is attached to the exam tab and survives navigation away
from the `grips://` bootstrap. Only primary main-frame navigations are reported:
commits, redirect hops, same-document navigation, failures and cancellations.
XHR, fetch, images, iframes and other subresources are not audited. Reporting
stops on leaving the exam; ordinary browsing is outside its scope. The student
bootstrap displays this scope before entering the exam.

Both native client and server remove URL credentials, the entire query string
and fragments. Non-HTTP(S) URLs are not stored (only the navigation category).
Paths longer than the URL cap fall back to the origin. HTTP paths themselves
are retained; origins must not put secret credentials in path components.

## Event model

- `exam_started` / `exam_completed`: existing authoritative server events.
- `exam_entered`: the native exam policy/audit observer was activated.
- `page_navigation`: sanitized URL, Chromium navigation ID, route, result,
  HTTP status and net error when available.
- `page_redirect`: sanitized source and destination, with the redirect hop's
  response metadata (not a guess based on the final domain).
- `exam_left`: background, tab closed, renderer exit, deadline, control-page
  return, policy clear, or replacement by another exam session.

Route comes from the actual navigation response proxy chain, including the
existing `byod-xhttp.invalid:1` marker. Values are `tunnel`, `direct`, `proxy`
(another/system proxy), `none`, or `unknown`. Cache/same-document/BFCache
responses are marked as no network; service-worker responses and failed
connections are not guessed to be direct/tunnel. This is client-observed
telemetry, not a tamper-proof attestation or an automatic policy violation.

`audit.observed_at_ms` is the client clock; enclosing `occurred_at` is server
receipt time. Both are shown in the admin log and session timeline.

## Delivery and storage

`POST /v1/sessions/{sessionId}/events` uses the session bearer token. The native
reporter pins its destination to `https://exam.cs.ac.cn`, bypasses the exam
tunnel, omits cookies/cache, and refuses redirects. The endpoint URL cannot be
selected by a source page. Only the trusted HTTPS bootstrap bridge supplies
`auditSessionId` / `auditToken` with the native configuration.

The endpoint validates an allowlist of event types/fields, limits requests to
64 KiB / 32 events and each cached session to 600 observations/minute. Durable
event IDs are derived from the authenticated session and client UUID, so retry
is idempotent even after a control restart. PostgreSQL insertion is atomic.
Migration adds nullable `byod_events.audit` JSONB; existing events are retained.

Audit does **not** update heartbeats, session state, violation counts or exam
completion. Recently ended sessions accept delayed events for up to 24 hours
after last heartbeat. The native queue is bounded to 256 pending events and
retries transient errors four times; failed delivery is logged without URLs
or tokens. The reporter can outlive its tab, but process shutdown, kill, power
loss or prolonged offline time can lose pending events. No credentials are
persisted to disk by this reporter. Heartbeat expiry remains the fallback;
it must not be presented as the exact time a student closed their browser.

## Build and validation

Chromium 155 follow-up (2026-10-08): the source baseline now incorporates the
original audit patch; apply the series in `patches/chromium-155/README.md`.
`1306-audit-tab-close.patch` distinguishes fast normal renderer shutdown during
tab close from a real renderer crash, and prevents crash-induced visibility
changes from being reported as backgrounding. The fresh native test passed
**22 audit events**, including deliberate renderer crash and actual tab close.
The signed 155 test installer is available; this does **not** deploy the
server audit API or prove live OIDC/exam acceptance.

Native delta: `patches/1303-page-audit.patch`, applied after the current XHTTP
changes. Only `grips_ui.cc` and `navigation_handle.cc` are compiled. A small
separate public helper avoids modifying the widely included NavigationHandle
interface or NetworkContext Mojo interfaces. Build settings/toolchain remain
unchanged. Run the Ninja dry run before `build/incremental-audit.cmd`.

The isolated Windows fixture and `build/smoke-native-audit.mjs` exercise actual
native reporting and ensure subresources/normal browsing/URL secrets are
excluded. They use synthetic sessions, a fresh profile, loopback SSH forwarding
and a test-only certificate SPKI; no production records or system trust change.

Deployment order: server migration/API + exam UI first, then the browser.
Older browser installers do not gain native page auditing from a server update.
No release, signing, packaging or production deployment is implied by source
changes or local test success.

Validated on 2026-10-07:

- Go race suite with an isolated PostgreSQL 17 instance and `go vet`: passed.
- Admin TypeScript/build, 11 unit tests and 9 Playwright UI tests: passed.
- Windows incremental build: initially 5 actions (two C++ objects and links);
  follow-up fixes each used 4 actions. No full rebuild or build-argument change.
- Build manifest integrity check and native Windows test: passed. The final
  test received 19 audit events and verified tunnel/direct, redirect, cache,
  same-document, failure, control-page leave, deadline expiry and tab close.
  It also verified exclusion of XHR/fetch/images, normal browsing after leave,
  and URL query/fragment secrets. The tab-close case keeps another isolated
  blank tab alive; last-window/process shutdown delivery remains best effort.
- No production deployment, new installer or signing was performed for this
  feature. Native output is `C:\src\chromium\src\out\Grips`; original source
  backups and logs remain in `C:\src\chromium\byod-audit-tools-20261007`.
