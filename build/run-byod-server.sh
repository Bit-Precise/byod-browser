#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export BYOD_POLICY_SECRET="${BYOD_POLICY_SECRET:-local-only-change-me}"
cd byod_server
exec go run ./cmd/byod-server "$@"
