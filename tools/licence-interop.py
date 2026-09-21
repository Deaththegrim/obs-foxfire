#!/usr/bin/env python3
"""Cross-language licence proof: Python signs, the C engine verifies.

The engine's unit tests sign with Monocypher and verify with Monocypher, so they
cannot catch a divergence between our two Ed25519 implementations. packforge
mints licences with python-cryptography, so THAT is the pairing that has to hold.
This drives the built verify_cli with real python-cryptography signatures across
every licence state, every tamper, and every JSON whitespace style a licence file
might reach a buyer in.

Usage: licence-interop.py <path to verify_cli>
Exit:  0 all checks passed, 1 a check failed, 77 skipped (ctest SKIP_RETURN_CODE).
"""
import base64
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

try:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
except ImportError:
    print("SKIP: python-cryptography is not installed")
    sys.exit(77)

if len(sys.argv) != 2:
    sys.exit(__doc__)
CLI = Path(sys.argv[1])
if not CLI.exists():
    print(f"SKIP: {CLI} was not built")
    sys.exit(77)


def canonical(discord_id, pack_id, licence_id, issued, expires):
    """Byte-for-byte what ff_licence_canonical() builds: sorted keys, no spaces."""
    return json.dumps(
        {
            "discord_id": discord_id,
            "expires": int(expires),
            "issued": int(issued),
            "licence_id": licence_id,
            "pack_id": pack_id,
        },
        sort_keys=True,
        separators=(",", ":"),
    ).encode()


def main():
    key = Ed25519PrivateKey.generate()
    pub_hex = (
        key.public_key()
        .public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        .hex()
    )
    now = int(time.time())
    fields = {
        "discord_id": "123456789012345678",
        "pack_id": "ember",
        "licence_id": "lic_interop",
        "issued": now - 86400,
        "expires": now + 90 * 86400,
    }
    sig = base64.b64encode(key.sign(canonical(**fields))).decode()
    doc = dict(fields, sig=sig)
    expires = fields["expires"]
    grace = 7 * 86400

    failures = []
    tmp = Path(tempfile.mkdtemp()) / "licence.json"

    def check(label, text, when, expected, expected_pack_id="ember", expected_reason=None):
        tmp.write_text(text)
        out = subprocess.run(
            [str(CLI), pub_hex, str(tmp), expected_pack_id, str(when)],
            capture_output=True,
            text=True,
            timeout=30,
        ).stdout.strip()
        got = out.split(" ", 1)[0] if out else "<no output>"
        ok = got == expected
        if ok and expected_reason is not None:
            ok = expected_reason in out
        print(f"{'PASS' if ok else 'FAIL'} {label}: {out}")
        if not ok:
            if expected_reason and expected_reason not in out:
                failures.append(f"{label}: expected {expected} with reason containing {expected_reason!r}, got {out}")
            else:
                failures.append(f"{label}: expected {expected}, got {got}")

    compact = json.dumps(doc, sort_keys=True, separators=(",", ":"))

    # The three licence states, driven only by the clock.
    check("valid, in date", compact, now, "OK")
    check("expired 3 days, inside grace", compact, expires + 3 * 86400, "GRACE")
    check("expired past grace", compact, expires + grace + 86400, "EXPIRED")

    # Every signed field is actually covered by the signature.
    for field, bad in [
        ("discord_id", "999999999999999999"),
        ("pack_id", "embyr"),
        ("licence_id", "lic_someone_else"),
        ("issued", fields["issued"] - 99999),
        ("expires", expires + 365 * 86400),
    ]:
        tampered = json.dumps(dict(doc, **{field: bad}), sort_keys=True, separators=(",", ":"))
        check(f"tampered {field}", tampered, now, "INVALID")

    other = Ed25519PrivateKey.generate()
    foreign = dict(fields, sig=base64.b64encode(other.sign(canonical(**fields))).decode())
    check(
        "signed by a foreign key",
        json.dumps(foreign, sort_keys=True, separators=(",", ":")),
        now,
        "INVALID",
    )

    # A genuine, validly-signed "ember" licence copied verbatim into a different paid pack's
    # folder (e.g. "smoke") must NOT verify there: pack_id is signed, so the signature itself
    # is fine -- only comparing it against the pack directory it was found in catches the copy.
    check(
        "valid licence for a different pack",
        compact,
        now,
        "INVALID",
        expected_pack_id="smoke",
    )

    # A licence is still a licence whatever serialiser formatted it. These are the
    # cases that regressed to "missing a field" before the parser skipped whitespace.
    check("json.dumps default separators", json.dumps(doc, sort_keys=True), now, "OK")
    check("pretty-printed, indent 2", json.dumps(doc, sort_keys=True, indent=2), now, "OK")
    check("tab-indented", json.dumps(doc, sort_keys=True, indent="\t"), now, "OK")
    check(
        "whitespace before the colon",
        ",".join(f'"{k}" : {json.dumps(v)}' for k, v in sorted(doc.items())).join("{}"),
        now,
        "OK",
    )

    print()
    if failures:
        print(f"{len(failures)} INTEROP CHECK(S) FAILED")
        for f in failures:
            print("  " + f)
        return 1
    print("all interop checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
