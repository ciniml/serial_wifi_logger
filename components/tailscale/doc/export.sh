#!/usr/bin/env bash
# Thin wrapper that runs the Node-based Marp build from any cwd.
# Usage:
#   ./export.sh           # build HTML and PDF
#   ./export.sh html      # HTML only
#   ./export.sh pdf       # PDF only
set -euo pipefail

cd "$(dirname "$0")"

if ! command -v node >/dev/null 2>&1; then
  echo "node is required" >&2
  exit 1
fi

node build.js "${1:-all}"
