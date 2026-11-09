#!/usr/bin/env bash
# Copy coldchaind, the dashboard and the trace helper to a QNX target.
#   TARGET=192.168.56.101 ./tools/deploy.sh [arch]     arch: x86_64 (default) | aarch64le
# Works from Linux, macOS, WSL or Git Bash on Windows.
set -euo pipefail
cd "$(dirname "$0")/.."
TARGET="${TARGET:?set TARGET=<target ip>}"
ARCH="${1:-x86_64}"
USER_AT="${TARGET_USER:-root}@${TARGET}"
BIN="build/${ARCH}/coldchaind"

[ -x "$BIN" ] || { echo "missing $BIN — run 'make qnx' (or 'make qnx-arm') first" >&2; exit 1; }

ssh "$USER_AT" "mkdir -p /data/coldchain"
scp -q "$BIN" tools/trace.sh "$USER_AT:/data/coldchain/"
scp -q -r web "$USER_AT:/data/coldchain/"
echo "deployed. on the target run:"
echo "  /data/coldchain/coldchaind --web /data/coldchain/web"
echo "then open http://${TARGET}:8080"
