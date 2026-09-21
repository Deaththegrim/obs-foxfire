#!/usr/bin/env bash
# Runs tools/ff_proof.py against a throwaway OBS in a fresh sandbox config dir.
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

# --multi: without it, a second OBS opens an "already running" warning dialog instead of
# starting. Under Xvfb nobody can click it, so the websocket port never opens and the proof
# fails with a bare ConnectionRefusedError that says nothing about the real cause. This bites
# whenever the developer happens to have their own OBS open, which is most of the time.
# Never connect to a previous run's dying OBS: every proof here uses the same fixed port, so a
# lingering instance would answer instead and the run would report on the wrong build. The Python
# harnesses call proof.wait_for_port_free() for the same reason.
for _ in $(seq 1 60); do
	python3 -c "import socket,sys; s=socket.socket(); sys.exit(0 if s.connect_ex(('127.0.0.1',$port)) else 1)" && break
	sleep 0.5
done

XDG_CONFIG_HOME="$sb" xvfb-run -a -s "-screen 0 1280x720x24" obs --multi --minimize-to-tray \
	>"$sb/obs-stdout.txt" 2>&1 &
obspid=$!
for _ in $(seq 1 40); do
	sleep 1
	python3 -c "import socket,sys; s=socket.socket(); sys.exit(s.connect_ex(('127.0.0.1',$port)))" && break
done

# a second clock around the driver: the driver times out each request, but if it hangs anywhere
# else (a connect that never resolves, an OBS that wedged before the socket opened) this is what
# turns the hang into a failure. FF_PROOF_TIMEOUT exists so this branch can be armed.
drv_timeout=${FF_PROOF_TIMEOUT:-300}
rc=0
timeout "$drv_timeout" python3 "$here/tools/ff_proof.py" "$port" || rc=$?
if [ "$rc" -eq 124 ]; then
	echo "  [FAIL] the render proof returns: driver killed after ${drv_timeout}s -- a step never came back, which is what a deadlock looks like"
fi

pkill -TERM -P "$obspid" 2>/dev/null || true
kill -TERM "$obspid" 2>/dev/null || true
# a source whose destroy deadlocks would hang here: give it room, then say so
for _ in $(seq 1 20); do kill -0 "$obspid" 2>/dev/null || break; sleep 1; done
if kill -0 "$obspid" 2>/dev/null; then
	echo "  [FAIL] obs exits on SIGTERM: still alive 20s after the signal -- a destroy-path deadlock looks like this"
	if [ "$rc" -eq 0 ]; then rc=1; fi
fi
kill -KILL "$obspid" 2>/dev/null || true
wait "$obspid" 2>/dev/null || true
cp "$sb"/obs-studio/logs/*.txt /tmp/ff-obs.log 2>/dev/null || cp "$sb/obs-stdout.txt" /tmp/ff-obs.log
echo "log: /tmp/ff-obs.log"
# The sandbox goes when the run passed: the log is already copied out, and these accumulate. Left
# in place on a failure, because that is when someone needs to look inside it -- forty-nine of
# them had piled up in /tmp before this existed, and the disk pressure that caused is what made
# the recording step start failing.
if [ "$rc" -eq 0 ]; then
	rm -rf "$sb"
else
	echo "sandbox kept for inspection: $sb"
fi
exit $rc
