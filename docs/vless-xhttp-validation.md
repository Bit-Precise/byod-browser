# VLESS/XHTTP validation — 2026-10-07

The implemented profile is VLESS v0 TCP, `encryption=none`, empty flow, over
XHTTP `packet-up`. It is not the complete set of Xray XHTTP modes. Source TLS
remains inside the stream; the transport is HTTPS to `/v1/xhttp/`.

## Completed checks

- `BYOD_XRAY_BINARY=/tmp/byod-vless-ciDU57/xray go test -race ./...`
  in `byod_server`: passed. The independently built Xray reference is commit
  `d2758a02`. Its client verifies both outer and source TLS using test CAs,
  transfers a 1.4 MB echo, and loses access after exam completion.
- Go tests cover a 1.8 MB source-TLS echo, upload fragmentation/reordering,
  invalid credentials, expiry, explicit host allowlisting, stream revocation,
  malformed requests, queue limits, duplicate downloads and shutdown cleanup.
  Revoking a UUID closes its existing stream even if the session stays active.
- `go vet ./...`, admin UI typecheck, exam UI TypeScript check, Helm lint,
  Helm rendering and `/root/k8s` TypeScript check: passed. The rendered chart
  exposes only HTTP service port 8787, not a dedicated tunnel port.
- Windows `out\Grips\chrome` incremental Ninja build and build-manifest
  verification: passed. `args.gn` and `toolchain.ninja` hashes are unchanged.
- Actual Windows Chromium native transport, with outer HTTP/1.1 forced:
  source page loaded; 12 concurrent echo requests / 2,949,120 bytes passed;
  clearing native policy left zero active server streams.
- The same native test with outer HTTP/2: passed, including stream cleanup.

Native test traffic used an isolated profile, loopback-only fixtures and SSH
forwarding. The launcher's certificate exception was restricted to the test
certificate's SPKI; no system trust store was changed. Production code retains
normal certificate validation. This test injects native configuration via CDP:
it validates the transport, not the complete OIDC/exam UI flow.

## Native test tools

`byod_server/tunnel_native_fixture_test.go` is opt-in and skipped by normal
tests. Set `BYOD_NATIVE_FIXTURE` to a new JSON file in a private temporary
directory and run:

```sh
go test -race -run '^TestNativeXHTTPFixture$' -v -timeout 11m .
```

The JSON describes temporary endpoints, UUID and test certificate. Forward its
HTTPS endpoint to Windows loopback and launch the verified build in an isolated
interactive-session profile with a separate CDP port. Resolve `exam.cs.ac.cn`
to that loopback endpoint only in the test browser; pin only the fixture SPKI.
Do not change system DNS, trust or existing browser profiles. Add
`--disable-http2` for the HTTP/1.1 case. Then run:

```sh
node build/smoke-native-xhttp.mjs /private/fixture.json CDP_PORT OUTER_PORT HTTP/1.1
# Fresh browser/profile without --disable-http2:
node build/smoke-native-xhttp.mjs /private/fixture.json CDP_PORT OUTER_PORT HTTP/2.0
```

The script closes only the browser on that isolated CDP port. Stop the fixture
through its authenticated `/fixture/stop` endpoint and remove its temporary
scheduled tasks/SSH forwarding after testing. It also expires after 10 minutes.

## Build and rollout boundaries

Build command: `third_party\ninja\ninja.exe -C out\Grips -j8 chrome`, preceded
by `-n chrome` inspection. No clean, build-argument change or toolchain switch
was used. A pre-existing corrupt `ax_relation_cache.obj` prevented linking;
it was preserved and rebuilt as one exact target. Its backup and test logs are
under `C:\src\chromium\byod-xhttp-tools-20261007`.

The browser changes are preserved in `patches/1302-vless-xhttp-tunnel.patch`.
Windows build output: `C:\src\chromium\src\out\Grips`.

## Split-role and installer validation

- Real isolated PostgreSQL tests with separate control/data connection pools
  passed under `go test -race` alongside the Xray interop suite (22.4 seconds).
- An empty data-plane session map accepts control-issued durable credentials.
  Control restarts do not interrupt data streams. Credential revocation,
  suspension, roster removal, policy removal, credential expiry, exam expiry,
  completion and database errors close established streams within 5 seconds.
- Durable heartbeat freshness is preserved across stale control writes;
  control idle suspension uses an atomic conditional update.
- Data role rejects login/admin/session APIs, needs no control-plane secrets,
  and does not perform OIDC discovery. Control role rejects XHTTP.
- Helm renders two single-replica Recreate Deployments and two Services; the
  specific XHTTP route targets the data Service, with no new public port.

Signed installer (already on Windows, not automatically installed):
`C:\Users\mryao\Downloads\BYOD-Browser-Setup-152.0.7947.0-xhttp-signed-20261007.exe`.
SHA-256: `186a383b90f3cdcb747e16544c6747f65e7809f5aa96bfc7f7088a64b31cb9dd`.
Azure Artifact Signing/RFC3161 verification passed: 12 own signed binaries,
11 unchanged vendor-signed binaries, 26 manifest hashes; canonical build unchanged.

## Deployment and CDN validation

`nj-prod` now runs image/chart `0.0.0-ci.80`, source commit `9f4dbc7`.
GitHub release workflow `37579374658` passed all jobs. Targeted `pulumi up`
updated the BYOD Helm release and HTTPRoute and deleted only the obsolete
raw-tunnel Service (8443/NodePort 30878); 396 unrelated resources were unchanged.
Both control/data Deployments are ready with one replica and Recreate strategy.
The Gateway route is Accepted/ResolvedRefs and `/healthz` reports role=control.

Real deployment tests used a temporary ClusterIP echo service and new synthetic
exam/identity/session records only (no migrations or student data changes).
They were removed afterward. The opt-in `TestDeployedXHTTP` test and
`build/run-deployed-xhttp-smoke.mjs` preserve the test procedure; they require an
explicit live database connection and must not be confused with isolated DB tests.

- Initial public EdgeOne path: trusted outer HTTPS/HTTP2, 2,160,000-byte echo succeeded;
  after about 15 seconds without downstream data, the GET was reset. The next
  POST was rejected because its transport session was already closed.
- Direct `11.0.11.2:443` dial with the **same exam.cs.ac.cn TLS hostname and
  normal certificate verification**: large echo succeeded, 65-second idle and
  resumed echo succeeded, durable liveness prevented idle suspension;
  inner `https://cs101.gbu.edu.cn/` certificate verification and HTTP 200 passed;
  credential revocation closed the established stream within 5 seconds.
- Read-only inspection of Istio's live Envoy config confirmed route timeout=0
  and stream_idle_timeout=0. The difference is the public EdgeOne path, whose
  origin/read idle settings are not managed in this Pulumi project.

### Public retest after operator increased EdgeOne timeout

2026-10-07 14:50 CST: `TestDeployedXHTTP` passed in 68.40 seconds through the
public EdgeOne path, with direct-dial override explicitly disabled. Verified:

- System-trusted outer HTTPS/HTTP2 and a 2,160,000-byte echo.
- A genuinely idle 65-second stream resumed successfully; durable liveness
  prevented a control-plane heartbeat timeout.
- Inner `https://cs101.gbu.edu.cn/` certificate verification and HTTP 200.
- Credential revocation closed the public stream within 5 seconds.
- Data-plane log recorded the download for 68,065 ms and 2,160,012 bytes.

The earlier roughly 15-second idle reset did not recur. This validates the
65-second idle case, not the exact new timeout ceiling or automatic reconnect
after exceeding it. Full interactive OIDC/exam browser workflow remains a
separate acceptance test. Temporary fixture records, Pod and Service were
removed, and the database port-forward exited. No application redeploy,
Chromium rebuild or agent-made CDN/DNS change was needed for this retest.
Data remains one replica until transport-session-aware routing is implemented;
a data-plane upgrade interrupts existing streams.
