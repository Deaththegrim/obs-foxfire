#!/usr/bin/env python3
"""Runs ff-twitch-api.c against a local stand-in for Twitch.

Nothing here reaches the network and nothing needs an account. The server answers with the
response shapes Twitch documents, and the C side runs its REAL code -- ff_twitch_set_bases only
moves the host. The failures this catches are the ones nobody can reproduce by hand: a scope list
with a duplicate in it, a form body that was not escaped, `authorization_pending` reported to the
streamer as an error every five seconds while they walk to their phone, and channel.follow
subscribed without the moderator_user_id its condition requires.

ARMED by mutation, each guard in ff-twitch-api.c disabled in turn:

    control (every guard in place)               20/20 passed
    scope de-duplication removed                 19/20
    scopes not percent-encoded                   19/20
    expiry stored as a duration, not a moment    19/20
    authorization_pending treated as an error    19/20
    moderator_user_id left out of the condition  19/20
    raid condition uses broadcaster_user_id      19/20
    Client-Id header dropped from helix          18/20
    interval floor removed                       19/20
    verification_uri fallback removed            19/20

The last two SURVIVED at first, at a clean 18/18: this server always answered with an interval
and a verification_uri, so neither fallback was ever reached. A mock that only sends complete
responses only tests the complete-response path. It now answers the second device call with
both fields missing, which is a shape the response is allowed to have.

    tools/twitch-api-proof.py <path to twitch_cli>
"""

import json
import subprocess
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

FAILS = []
CHECKS = []
SEEN = {"subscribe": []}


def check(name, ok, detail=""):
    CHECKS.append(name)
    if not ok:
        FAILS.append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail else ""))


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/helix/users":
            SEEN["users_auth"] = self.headers.get("Authorization", "")
            SEEN["users_cid"] = self.headers.get("Client-Id", "")
            self._send(200, {"data": [{"id": "4242", "login": "junkie",
                                       "display_name": "junkie"}]})
        else:
            self._send(404, {"message": "no"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n).decode()
        if self.path == "/oauth2/device":
            SEEN["device_body"] = raw
            form = urllib.parse.parse_qs(raw)
            SEEN["device_scopes"] = form.get("scopes", [""])[0]
            SEEN["device_calls"] = SEEN.get("device_calls", 0) + 1
            body = {"device_code": "dev123", "expires_in": 1800, "user_code": "ABCD1234",
                    "verification_uri": "https://www.twitch.tv/activate"}
            # The second call answers with NO interval and NO verification_uri. Both are things a
            # response can leave out, and both have a floor/default in the client that nothing
            # exercised until this existed -- the mutation that removed the interval floor
            # scored a clean 18/18.
            if SEEN["device_calls"] == 1:
                body["interval"] = 5
            else:
                del body["verification_uri"]
            self._send(200, body)
        elif self.path == "/oauth2/token":
            form = urllib.parse.parse_qs(raw)
            grant = form.get("grant_type", [""])[0]
            if grant == "refresh_token":
                SEEN["refresh_body"] = raw
                self._send(200, {"access_token": "at-refreshed", "refresh_token": "rt-new",
                                 "expires_in": 14400, "scope": ["bits:read"],
                                 "token_type": "bearer"})
                return
            SEEN["token_body"] = raw
            SEEN["token_grant"] = grant
            # The device flow's normal answer until the streamer finishes. Twitch returns it as a
            # 400 with a message, NOT as a success, which is exactly why it is easy to mishandle.
            stage = SEEN.get("stage", "pending")
            if stage == "pending":
                SEEN["stage"] = "granted"
                self._send(400, {"status": 400, "message": "authorization_pending"})
            else:
                self._send(200, {"access_token": "at-fresh", "refresh_token": "rt-fresh",
                                 "expires_in": 14400, "scope": ["bits:read"],
                                 "token_type": "bearer"})
        elif self.path == "/helix/eventsub/subscriptions":
            body = json.loads(raw)
            SEEN["subscribe"].append(body)
            SEEN["sub_ct"] = self.headers.get("Content-Type", "")
            self._send(202, {"data": [{"id": "s1", "status": "enabled"}]})
        else:
            self._send(404, {"message": "no"})


def run(cli, base, step):
    r = subprocess.run([cli, base, step], capture_output=True, text=True, timeout=60)
    return r.stdout.strip()


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cli = sys.argv[1]

    srv = HTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{srv.server_port}"

    # ---- the scope list ----
    scopes = run(cli, base, "scopes").replace("SCOPES:", "").split()
    check("every scope the subscriptions need is asked for",
          {"moderator:read:followers", "channel:read:subscriptions", "bits:read",
           "channel:read:redemptions"} <= set(scopes), " ".join(scopes))
    check("no scope is asked for twice",
          len(scopes) == len(set(scopes)),
          f"{len(scopes)} asked, {len(set(scopes))} distinct -- Twitch rejects a duplicated list")

    # ---- the device code ----
    out = run(cli, base, "device")
    check("the device flow returns a code the streamer can type",
          "CODE:ABCD1234" in out and "twitch.tv/activate" in out, out)
    check("it does not poll faster than Twitch allows", "INTERVAL:5" in out, out)
    bare = run(cli, base, "device")
    check("a response with no interval still does not poll in a tight loop",
          "INTERVAL:5" in bare,
          "Twitch documents 5 seconds; 0 would be rate limited into looking like a broken login")
    check("a response with no verification_uri still tells them where to go",
          "twitch.tv/activate" in bare, bare)

    body = SEEN.get("device_body", "")
    check("the scope list is percent-encoded in the form body",
          "%3A" in body and " " not in body,
          "a raw colon or space produces a 400 that reads like a credentials problem")

    # ---- polling, which answers 'not yet' first ----
    first = run(cli, base, "poll")
    check("'authorization_pending' is reported as PENDING, not as an error",
          first.startswith("POLL:PENDING"), first)
    second = run(cli, base, "poll")
    check("and the token is picked up once they authorise",
          second.startswith("POLL:GOT_TOKEN") and "ACCESS:at-fresh" in second, second)
    check("the token's expiry is stored as a moment, not a duration",
          "TTL_IN_FUTURE:1" in second, second)
    check("the device grant type is the one the flow requires",
          SEEN.get("token_grant") == "urn:ietf:params:oauth:grant-type:device_code",
          SEEN.get("token_grant", ""))

    # ---- refresh ----
    out = run(cli, base, "refresh")
    check("a saved sign-in can be refreshed", out.startswith("REFRESH:1") and "at-refreshed" in out,
          out)

    # ---- who we are ----
    out = run(cli, base, "user")
    check("the broadcaster's numeric id is read, not their name",
          "ID:4242" in out and "LOGIN:junkie" in out, out)
    check("helix is called with both the bearer token and the client id",
          SEEN.get("users_auth") == "Bearer at123" and SEEN.get("users_cid") == "cid123",
          f"auth={SEEN.get('users_auth')!r} client-id={SEEN.get('users_cid')!r}")

    # ---- the subscriptions ----
    out = run(cli, base, "subscribe")
    subs = SEEN["subscribe"]
    check("every subscription is accepted", out.count("OK:1") == len(subs) and len(subs) > 0,
          f"{len(subs)} sent")
    check("all seven are asked for", len(subs) == 7, f"{len(subs)}")
    by_type = {s["type"]: s for s in subs}
    follow = by_type.get("channel.follow", {})
    check("channel.follow is version 2 with moderator_user_id in its condition",
          follow.get("version") == "2" and
          "moderator_user_id" in follow.get("condition", {}),
          "version 1 is removed, and a condition without the moderator id is refused -- the "
          "alert type then simply never fires")
    raid = by_type.get("channel.raid", {})
    check("channel.raid's condition is about who RECEIVES the raid",
          "to_broadcaster_user_id" in raid.get("condition", {}),
          str(raid.get("condition")))
    check("every subscription rides the open websocket session",
          all(s["transport"]["method"] == "websocket" and
              s["transport"]["session_id"] == "sess1" for s in subs))
    check("they are sent as JSON", SEEN.get("sub_ct", "").startswith("application/json"),
          SEEN.get("sub_ct", ""))

    srv.shutdown()
    print(f"\ntwitch-api proof: {len(CHECKS) - len(FAILS)}/{len(CHECKS)} passed")
    if not CHECKS:
        print("twitch-api proof: NOTHING INSPECTED")
        return 2
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
