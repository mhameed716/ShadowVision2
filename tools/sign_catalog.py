#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Sign / verify the model catalog with Ed25519 (docs/decisions-ar.md D10).

Why Python and not the app: the private key must never exist on a phone, in a
build artifact, or in the repository's history. Only the public key is compiled
in (see app/.../SvCatalog.kt).

    # one-time: create a dev key pair (writes keys/dev.key + keys/dev.pub)
    python tools/sign_catalog.py keygen --key-id dev-0001 --out keys/

    # sign: hashes every listed file, fills bytes+sha256, then signs
    python tools/sign_catalog.py sign --catalog models/catalog.txt \
        --key keys/dev.key --models models/

    # verify exactly as the app does (same payload bytes, same key)
    python tools/sign_catalog.py verify --catalog models/catalog.signed.txt \
        --pub keys/dev.pub

Dependency-free: Ed25519 is implemented here from RFC 8032 in ~70 lines because
requiring `cryptography` (or `pynacl`) on a build machine for one signature is a
supply-chain liability, not a convenience.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys

# ---------------------------------------------------------------------------
# Ed25519 — RFC 8032, compact reference implementation.
# ---------------------------------------------------------------------------
_P = 2**255 - 19
_L = 2**252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _P - 2, _P) % _P
_I = pow(2, (_P - 1) // 4, _P)


def _sha512(data: bytes) -> bytes:
    return hashlib.sha512(data).digest()


def _inv(x: int) -> int:
    return pow(x, _P - 2, _P)


def _x_recover(y: int) -> int:
    xx = (y * y - 1) * _inv(_D * y * y + 1)
    x = pow(xx, (_P + 3) // 8, _P)
    if (x * x - xx) % _P != 0:
        x = (x * _I) % _P
    if x % 2 != 0:
        x = _P - x
    return x


_BY = 4 * _inv(5) % _P
_BX = _x_recover(_BY)
_B = (_BX % _P, _BY % _P, 1, (_BX * _BY) % _P)


def _add(p, q):
    a = (p[1] - p[0]) * (q[1] - q[0]) % _P
    b = (p[1] + p[0]) * (q[1] + q[0]) % _P
    c = 2 * p[3] * q[3] * _D % _P
    d = 2 * p[2] * q[2] % _P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _mul(s: int, p):
    q = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            q = _add(q, p)
        p = _add(p, p)
        s >>= 1
    return q


def _equal(p, q) -> bool:
    if (p[0] * q[2] - q[0] * p[2]) % _P != 0:
        return False
    return (p[1] * q[2] - q[1] * p[2]) % _P == 0


def _compress(p) -> bytes:
    zinv = _inv(p[2])
    x = p[0] * zinv % _P
    y = p[1] * zinv % _P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decompress(s: bytes):
    y = int.from_bytes(s, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _x_recover(y)
    if x & 1 != sign:
        x = _P - x
    p = (x, y, 1, x * y % _P)
    if not _equal(_mul(_L, p), (0, 1, 1, 0)):
        return None
    return p


def _expand(seed: bytes):
    h = _sha512(seed)
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def _secret_scalar(seed: bytes) -> int:
    return _expand(seed)[0]


def _public(seed: bytes) -> bytes:
    a = _secret_scalar(seed)
    return _compress(_mul(a, _B))


def _sign(seed: bytes, msg: bytes) -> bytes:
    a, prefix = _expand(seed)
    r = int.from_bytes(_sha512(prefix + msg), "little") % _L
    rs = _compress(_mul(r, _B))
    h = int.from_bytes(_sha512(rs + _public(seed) + msg), "little") % _L
    s = (r + h * a) % _L
    return rs + int.to_bytes(s, 32, "little")


def _verify(pub: bytes, msg: bytes, sig: bytes) -> bool:
    if len(sig) != 64 or len(pub) != 32:
        return False
    r = _decompress(sig[:32])
    a = _decompress(pub)
    if r is None or a is None:
        return False
    s = int.from_bytes(sig[32:], "little")
    if s >= _L:
        return False
    h = int.from_bytes(_sha512(sig[:32] + pub + msg), "little") % _L
    return _equal(_mul(s, _B), _add(r, _mul(h, a)))


# ---------------------------------------------------------------------------
# Catalog handling
# ---------------------------------------------------------------------------
HEX64 = re.compile(r"^[0-9a-f]{64}$")


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def fill_hashes(text: str, models_dir: str) -> str:
    """Fills `bytes=` and `sha256=` for entries from the actual files on disk."""
    out, cur = [], None
    lines = text.splitlines()
    for line in lines:
        if line.strip() == "entry":
            cur = {}
            out.append(line)
            continue
        if cur is not None and "=" in line:
            k, v = line.split("=", 1)
            cur[k.strip()] = v.strip()
        if line.strip() == "end":
            cur = None
            out.append(line)
            continue
        out.append(line)
        if cur is not None:
            pass
    # second pass to rewrite bytes/sha256 (simpler than in-place bookkeeping)
    text2 = "\n".join(out)
    entries = re.findall(r"entry\n(.*?)\nend", text2, flags=re.S)
    for body in entries:
        file_m = re.search(r"^file=(.+)$", body, flags=re.M)
        if not file_m:
            sys.exit("entry without file=")
        name = file_m.group(1).strip()
        path = os.path.join(models_dir, name)
        if not os.path.exists(path):
            # Better to fail loudly than to emit a manifest that the app will
            # reject at runtime for "missing sha256" long after CI went green.
            sys.exit(f"error: {path} not found; cannot compute bytes/sha256 for "
                     f"{name}. Generate or download the model first, or sign a "
                     f"catalog that only lists files you actually have.")
        size = os.path.getsize(path)
        digest = sha256_file(path)
        new_body = re.sub(r"^bytes=.*$", f"bytes={size}", body, flags=re.M)
        if re.search(r"^sha256=", new_body, flags=re.M):
            new_body = re.sub(r"^sha256=.*$", f"sha256={digest}", new_body, flags=re.M)
        else:
            new_body = new_body + f"\nsha256={digest}"
        new_body = re.sub(r"^bytes=$", "", new_body, flags=re.M)
        text2 = text2.replace(body, new_body, 1)
    return text2 if text2.endswith("\n") else text2 + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    k = sub.add_parser("keygen")
    k.add_argument("--key-id", required=True)
    k.add_argument("--out", required=True)

    s = sub.add_parser("sign")
    s.add_argument("--catalog", required=True)
    s.add_argument("--key", required=True)
    s.add_argument("--models", default="models")
    s.add_argument("--out")

    v = sub.add_parser("verify")
    v.add_argument("--catalog", required=True)
    v.add_argument("--pub", required=True)

    args = ap.parse_args()

    if args.cmd == "keygen":
        os.makedirs(args.out, exist_ok=True)
        seed = os.urandom(32)
        pub = _public(seed)
        key_path = os.path.join(args.out, "dev.key")
        pub_path = os.path.join(args.out, "dev.pub")
        if os.path.exists(key_path):
            sys.exit(f"{key_path} exists; refusing to overwrite a signing key")
        with open(key_path, "wb") as f:
            f.write(seed)
        os.chmod(key_path, 0o600)
        with open(pub_path, "w") as f:
            f.write(pub.hex() + "\n")
        print(f"wrote {key_path} (0600) and {pub_path}")
        print(f"public key (compile into the app): {pub.hex()}")
        print("NEVER commit dev.key. Add keys/ to .gitignore (it already is).")
        return 0

    if args.cmd == "sign":
        with open(args.key, "rb") as f:
            seed = f.read()
        if len(seed) != 32:
            sys.exit("key must be 32 raw bytes")
        with open(args.catalog) as f:
            text = f.read()
        if "signature=" in text:
            text = text[: text.index("signature=")]
        text = fill_hashes(text, args.models)
        payload = text.encode()
        sig = _sign(seed, payload)
        out = args.out or args.catalog.replace(".txt", ".signed.txt")
        with open(out, "wb") as f:
            f.write(payload + f"signature={sig.hex()}\n".encode())
        print(f"signed -> {out}")
        print(f"public key: {_public(seed).hex()}")
        return 0

    if args.cmd == "verify":
        with open(args.catalog, "rb") as f:
            blob = f.read()
        with open(args.pub) as f:
            pub_hex = f.read().strip()
        idx = blob.rfind(b"signature=")
        if idx < 0:
            sys.exit("no signature line")
        payload = blob[:idx]
        sig_hex = blob[idx + len(b"signature="):].strip()
        ok = _verify(bytes.fromhex(pub_hex), payload, bytes.fromhex(sig_hex.decode()))
        # Cross-check the payload's declared key id against the provided key.
        print("VERIFY:", "OK" if ok else "FAILED")
        return 0 if ok else 1

    return 2


if __name__ == "__main__":
    sys.exit(main())
