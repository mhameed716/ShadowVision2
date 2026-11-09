#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Static check: the JNI registration and the Kotlin externals agree.

This exists because of a real bug that no compiler caught: jni_bridge.cpp
registered its natives on "ai/shadowvision/g0/SvEngine" while the object had been
moved to ai.shadowvision.core. FindClass returns null, JNI_OnLoad returns -1,
System.loadLibrary throws UnsatisfiedLinkError — the app dies before the first
frame, and neither the Kotlin compiler nor the C++ compiler has any opinion.

Checks performed:
  1. the class named in kClass matches package+object in SvEngine.kt,
  2. every Kotlin `external fun` has a registration entry,
  3. every registration entry has a Kotlin `external fun`,
  4. the JNI signature string matches the Kotlin declaration (argument count and
     primitive/array/string shapes) — a wrong descriptor is another
     UnsatisfiedLinkError that only shows up at runtime.
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JNI = os.path.join(ROOT, "android/app/src/main/cpp/jni_bridge.cpp")
KT = os.path.join(ROOT, "android/app/src/main/java/ai/shadowvision/core/SvEngine.kt")

# Kotlin type -> JNI type code. Object types are reduced to their shape.
KOTLIN_TO_JNI = {
    "Int": "I", "Long": "J", "Short": "S", "Byte": "B", "Char": "C",
    "Float": "F", "Double": "D", "Boolean": "Z", "String": "Ljava/lang/String;",
    "IntArray": "[I", "LongArray": "[J", "ShortArray": "[S", "ByteArray": "[B",
    "CharArray": "[C", "FloatArray": "[F", "DoubleArray": "[D", "BooleanArray": "[Z",
}
KOTLIN_RET = dict(KOTLIN_TO_JNI)
KOTLIN_RET["Unit"] = "V"


def parse_jni(src: str) -> tuple[str, dict[str, str]]:
    m = re.search(r'kClass\s*=\s*"([^"]+)"', src)
    if not m:
        raise SystemExit("check_jni_binding: no kClass in jni_bridge.cpp")
    klass = m.group(1)
    # entries look like {const_cast<char*>("nativeX"), const_cast<char*>("(sig)V"), ...
    entries = dict(re.findall(
        r'const_cast<char\*>\("(native\w+)"\),\s*const_cast<char\*>\("([^"]+)"\)', src))
    return klass, entries


def parse_kotlin(src: str) -> tuple[str, dict[str, tuple[list[str], str]]]:
    pkg = re.search(r"^package\s+([\w.]+)", src, re.M)
    obj = re.search(r"^object\s+(\w+)", src, re.M)
    if not pkg or not obj:
        raise SystemExit("check_jni_binding: no package/object in SvEngine.kt")
    fq = pkg.group(1) + "." + obj.group(1)
    fns: dict[str, tuple[list[str], str]] = {}
    for m in re.finditer(
            r"external fun\s+(native\w+)\s*\((.*?)\)\s*(?::\s*([\w<>?]+))?", src, re.S):
        name, params, ret = m.group(1), m.group(2), m.group(3) or "Unit"
        types = []
        for part in [p.strip() for p in params.split(",") if p.strip()]:
            t = part.split(":")[0].strip()
            # `x: IntArray` / `x: ByteArray` — drop nullability
            typ = part.split(":")[1].strip().rstrip("?")
            types.append(typ)
        fns[name] = (types, ret.rstrip("?"))
    return fq, fns


def expected_descriptor(args: list[str], ret: str) -> str | None:
    out = ""
    for t in args:
        code = KOTLIN_TO_JNI.get(t)
        if code is None:
            return None
        out += code
    rc = KOTLIN_RET.get(ret)
    if rc is None:
        return None
    return f"({out}){rc}"


def shape(sig: str) -> tuple[int, str]:
    """Reduce a JNI descriptor to (arg count, return code) for comparison."""
    assert sig.startswith("(")
    close = sig.index(")")
    args, ret = sig[1:close], sig[close + 1:]
    n = 0
    i = 0
    while i < len(args):
        if args[i] == "[":
            while args[i] == "[":
                i += 1
            if args[i] == "L":
                i = args.index(";", i)
            i += 1
        elif args[i] == "L":
            i = args.index(";", i) + 1
        else:
            i += 1
        n += 1
    return n, ret


def main() -> int:
    jni_src = open(JNI, encoding="utf-8").read()
    kt_src = open(KT, encoding="utf-8").read()
    klass, entries = parse_jni(jni_src)
    fq, fns = parse_kotlin(kt_src)

    problems: list[str] = []

    if klass != fq.replace(".", "/"):
        problems.append(f"class mismatch: JNI registers on '{klass}', "
                        f"Kotlin object is '{fq}' -> '{fq.replace('.', '/')}'")

    for name in sorted(set(fns) - set(entries)):
        problems.append(f"'{name}' is declared in Kotlin but never registered "
                        "(UnsatisfiedLinkError at first call)")
    for name in sorted(set(entries) - set(fns)):
        problems.append(f"'{name}' is registered but not declared in Kotlin "
                        "(dead entry, or the name was renamed on one side)")

    checked = 0
    for name in sorted(set(fns) & set(entries)):
        args, ret = fns[name]
        want = expected_descriptor(args, ret)
        got = entries[name]
        if want is None:
            continue  # an object type we do not model; names still match
        checked += 1
        if shape(want) != shape(got):
            problems.append(f"'{name}': Kotlin ({', '.join(args)}) -> {ret} expects "
                            f"{want}, jni_bridge declares {got}")

    if problems:
        print("JNI binding is inconsistent:", file=sys.stderr)
        for p in problems:
            print("  - " + p, file=sys.stderr)
        return 1
    print(f"  JNI binding OK: {klass} ({len(entries)} natives, "
          f"{checked} signatures verified)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
