#!/usr/bin/env bash
# Install Chromium depot_tools into $DEPOT_TOOLS_DIR (default /opt/depot_tools).
# Idempotent: safe to re-run.
set -euo pipefail

DEPOT_TOOLS_DIR="${DEPOT_TOOLS_DIR:-/opt/depot_tools}"
DEPOT_TOOLS_URL="https://chromium.googlesource.com/chromium/tools/depot_tools.git"

if [ -d "$DEPOT_TOOLS_DIR/.git" ]; then
  echo "[info] depot_tools already at $DEPOT_TOOLS_DIR, updating..."
  git -C "$DEPOT_TOOLS_DIR" fetch --depth=1 origin main
  git -C "$DEPOT_TOOLS_DIR" reset --hard origin/main
else
  echo "[info] cloning depot_tools into $DEPOT_TOOLS_DIR"
  parent_dir="$(dirname "$DEPOT_TOOLS_DIR")"
  mkdir -p "$parent_dir"
  git clone --depth=1 "$DEPOT_TOOLS_URL" "$DEPOT_TOOLS_DIR"
fi

# Sanity check
if [ ! -x "$DEPOT_TOOLS_DIR/gclient" ]; then
  echo "[error] gclient not found in $DEPOT_TOOLS_DIR after install"
  exit 1
fi

cat <<EOF

[success] depot_tools installed at $DEPOT_TOOLS_DIR

Add to your shell profile:
    export PATH="\$PATH:$DEPOT_TOOLS_DIR"
    export DEPOT_TOOLS_UPDATE=1

Then run: gclient --version
EOF
