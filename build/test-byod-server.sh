#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cd byod_server
export GOCACHE="${GOCACHE:-/tmp/byod-go-cache}"
go test ./...
go vet ./...
