#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../byod_server"
export GOCACHE="${GOCACHE:-/tmp/byod-go-cache}"
export BYOD_POLICY_SECRET="${BYOD_POLICY_SECRET:-local-smoke-secret}"

listen="${BYOD_SMOKE_LISTEN:-127.0.0.1:18787}"
base="http://${listen}"

go run ./cmd/byod-server \
  --listen "$listen" \
  --exam-origin "$base" \
  --upstream http://127.0.0.1:9 \
  --dev-auth >/tmp/byod-server-smoke.log 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true' EXIT

ready=0
for _ in $(seq 1 30); do
  if curl -fsS "$base/healthz" >/dev/null; then ready=1; break; fi
  sleep 0.2
done
if [[ "$ready" != 1 ]]; then
  echo "BYOD server did not become ready; see /tmp/byod-server-smoke.log" >&2
  exit 1
fi

python3 - "$base" <<'PY'
import json, sys, urllib.error, urllib.parse, urllib.request

base = sys.argv[1]
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None
opener = urllib.request.build_opener(NoRedirect)
def request(method, path, body=None, headers=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(base + path, data=data, method=method, headers=headers or {})
    if data is not None: req.add_header("Content-Type", "application/json")
    try:
        with opener.open(req) as response:
            return response.status, response.headers, json.loads(response.read() or b"{}")
    except urllib.error.HTTPError as error:
        if error.code in (301, 302, 303, 307, 308):
            return error.code, error.headers, {}
        return error.code, error.headers, json.loads(error.read() or b"{}")

status, _, config = request("GET", "/course-101/.well-known/byod-configuration")
assert status == 200 and config["policy"]["document"]["browser"]["allow_devtools"] is False
status, _, session = request("POST", "/v1/sessions", {"exam_id": "course-101"})
assert status == 201
sid, token = session["session_id"], session["browser_token"]
auth = urllib.parse.urlparse(session["authorization_url"])
status, headers, _ = request("GET", auth.path + "?" + auth.query)
assert status == 303
callback = urllib.parse.urlparse(headers["Location"])
status, _, _ = request("GET", callback.path + "?" + callback.query)
assert status in (200, 303)
headers = {"Authorization": "Bearer " + token}
status, _, started = request("POST", f"/v1/sessions/{sid}/start", {}, headers)
assert status == 200 and started["state"] == "active"
status, _, heartbeat = request("POST", f"/v1/sessions/{sid}/heartbeat", None, headers)
assert status == 200 and heartbeat["state"] == "active"
status, _, violation = request("POST", f"/v1/sessions/{sid}/violations", {"type": "background"}, headers)
assert status == 200 and violation["state"] == "suspended"
status, _, events = request("GET", f"/v1/sessions/{sid}/events", None, headers)
assert status == 200 and any(event["type"] == "background" for event in events["events"])
print("BYOD server smoke test passed")
PY
