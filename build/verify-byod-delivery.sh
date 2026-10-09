#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
export GOCACHE="${GOCACHE:-/tmp/byod-go-cache}"

(cd "$root/byod_server" && go test -run '^$' ./... && go vet ./...)
helm lint "$root/byod_server/helm/byod-server"
helm template byod "$root/byod_server/helm/byod-server" >/tmp/byod-server-rendered.yaml
grep -q 'kind: Deployment' /tmp/byod-server-rendered.yaml
grep -q 'path: /healthz' /tmp/byod-server-rendered.yaml
grep -q 'path: /readyz' /tmp/byod-server-rendered.yaml
echo "BYOD delivery verification passed"
