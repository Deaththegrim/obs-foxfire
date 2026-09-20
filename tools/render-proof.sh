#!/usr/bin/env bash
# Runs tools/render-proof.py against a throwaway OBS in a fresh sandbox config dir.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
port=4460
sb=$(mktemp -d /tmp/ff-sandbox.XXXX)
mkdir -p "$sb/obs-studio/plugin_config/obs-websocket"
cat > "$sb/obs-studio/plugin_config/obs-websocket/config.json" <<JSON
{"first_load": false, "server_enabled": true, "server_port": $port, "alerts_enabled": false, "auth_required": false, "server_password": ""}
JSON
# skips the first-run wizard, which otherwise owns the UI thread for the whole session
printf '[General]\nFirstRun=true\nConfirmOnExit=false\n' > "$sb/obs-studio/user.ini"
"$here/install-local.sh" "$sb/obs-studio" >/dev/null
echo "sandbox: $sb"

XDG_CONFIG_HOME="$sb" xvfb-run -a -s "-screen 0 1280x720x24" obs --minimize-to-tray \
	>"$sb/obs-stdout.txt" 2>&1 &
obspid=$!
for _ in $(seq 1 40); do
	sleep 1
	python3 -c "import socket,sys; s=socket.socket(); sys.exit(s.connect_ex(('127.0.0.1',$port)))" && break
done

rc=0
python3 "$here/tools/render-proof.py" "$port" || rc=$?

pkill -TERM -P "$obspid" 2>/dev/null || true
kill -TERM "$obspid" 2>/dev/null || true
# a source whose destroy deadlocks would hang here: give it room, then say so
for _ in $(seq 1 20); do kill -0 "$obspid" 2>/dev/null || break; sleep 1; done
kill -0 "$obspid" 2>/dev/null && echo "WARNING: obs did not exit within 20s of SIGTERM"
kill -KILL "$obspid" 2>/dev/null || true
wait "$obspid" 2>/dev/null || true
cp "$sb"/obs-studio/logs/*.txt /tmp/ff-obs.log 2>/dev/null || cp "$sb/obs-stdout.txt" /tmp/ff-obs.log
echo "log: /tmp/ff-obs.log"
exit $rc
