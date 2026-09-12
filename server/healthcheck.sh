#!/bin/sh
set -eu

pidfile="${USBIP_PIDFILE:-/run/usbip/usbipd.pid}"
[ -s "$pidfile" ] || exit 1
pid=$(sed -n '1p' "$pidfile")
case "$pid" in
    ''|*[!0-9]*) exit 1 ;;
esac
kill -0 "$pid" 2>/dev/null || exit 1

# This confirms that the USB/IP userspace tool can enumerate the host bus.
# It does not claim that a particular Windows client or USB application works.
usbip list -l >/dev/null 2>&1

# The single-port gateway (external port) must accept connections.
python3 - "${USBIP_PORT:-5555}" <<'PY'
import socket
import sys

with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=3) as s:
    s.close()
PY

if [ "${USBIP_WEB_ENABLED:-true}" = "true" ] || [ "${USBIP_WEB_ENABLED:-true}" = "1" ]; then
    python3 - "${USBIP_WEB_PORT:-8080}" <<'PY'
import json
import sys
import urllib.request

port = int(sys.argv[1])
with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/health", timeout=3) as response:
    payload = json.loads(response.read().decode("utf-8"))
    if response.status != 200 or not payload.get("ok"):
        raise SystemExit(1)
PY
fi
