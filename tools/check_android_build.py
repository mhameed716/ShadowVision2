#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Static audit of the Android build files, runnable without a JDK.

Every check here corresponds to a bug that *was* in this repository:

  * kotlinx.coroutines imports with no dependency on the classpath  -> nothing
    compiles, and the failure is a wall of "unresolved reference: launch";
  * an AppCompatActivity with no androidx.appcompat dependency -> ClassNotFound
    at runtime, on the one screen nobody opened yet;
  * a release buildType wired to the debug signing config -> an APK that looks
    shippable, expires, and cannot be updated by a properly signed build;
  * Compose plugin declared on the app module but not on the classpath;
  * an NDK/COMPILE SDK mismatch with the CMake toolchain version;
  * AGP older than the NDK r28 requirement for 16 KB page alignment.

It is deliberately text-based: the goal is to fail fast and explain, not to
reimplement Gradle.
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "android/app/build.gradle.kts")
ROOT_GRADLE = os.path.join(ROOT, "android/build.gradle.kts")
KT_DIRS = [os.path.join(ROOT, "android/app/src/main/java")]

problems: list[str] = []
notes: list[str] = []


def read(path: str) -> str:
    return open(path, encoding="utf-8").read()


app = read(APP)
root = read(ROOT_GRADLE)

# ---------------------------------------------------------------- kotlin deps
imports = set()
for base in KT_DIRS:
    for dirpath, _, files in os.walk(base):
        for f in files:
            if f.endswith(".kt"):
                src = read(os.path.join(dirpath, f))
                for m in re.finditer(r"^import\s+([\w.]+)", src, re.M):
                    imports.add(m.group(1))

def dep_declared(prefix: str) -> bool:
    return prefix in app

if any(i.startswith("kotlinx.coroutines") for i in imports) and \
        "kotlinx-coroutines-android" not in app:
    problems.append("kotlin code imports kotlinx.coroutines but "
                    "org.jetbrains.kotlinx:kotlinx-coroutines-android is not declared")

if any(i.startswith("androidx.appcompat") for i in imports) and \
        "androidx.appcompat:appcompat" not in app:
    problems.append("kotlin code imports androidx.appcompat but the appcompat "
                    "dependency is not declared")

if any(i.startswith("androidx.lifecycle.compose") for i in imports) and \
        "lifecycle-runtime-compose" not in app:
    problems.append("collectAsStateWithLifecycle needs androidx.lifecycle:"
                    "lifecycle-runtime-compose")

if any(i.startswith("androidx.exifinterface") for i in imports) and \
        "exifinterface" not in app:
    problems.append("kotlin code imports androidx.exifinterface but the dependency "
                    "is not declared")

if any(i.startswith("org.json") for i in imports) and "org.json" not in app:
    notes.append("org.json is part of the Android framework (no dependency needed)")

# ---------------------------------------------------------------- compose
if "compose = true" in app and "org.jetbrains.kotlin.plugin.compose" not in app:
    problems.append("buildFeatures.compose = true but the Compose compiler plugin "
                    "is not applied")
if "compose = true" in app and "compose-bom" not in app:
    problems.append("Compose is enabled but no Compose BOM is declared")

# ---------------------------------------------------------------- signing
if re.search(r"signingConfig\s*=\s*signingConfigs\.getByName\(\"debug\"\)\s*//", app):
    problems.append("release buildType is hard-wired to the debug signing config")
if "SV_KEYSTORE_FILE" not in app:
    problems.append("release signing does not read credentials from the environment")

# ---------------------------------------------------------------- sdk / ndk
m_compile = re.search(r"compileSdk\s*=\s*(\d+)", app)
m_target = re.search(r"targetSdk\s*=\s*(\d+)", app)
m_min = re.search(r"minSdk\s*=\s*(\d+)", app)
m_ndk = re.search(r'ndkVersion\s*=\s*"([\d.]+)"', app)
m_cmake = re.search(r'version\s*=\s*"([\d.]+)"', app)
if not (m_compile and m_target and m_min and m_ndk):
    problems.append("could not read compileSdk/targetSdk/minSdk/ndkVersion")
else:
    cs, ts, mins = int(m_compile.group(1)), int(m_target.group(1)), int(m_min.group(1))
    if ts > cs:
        problems.append(f"targetSdk {ts} > compileSdk {cs}")
    if mins > ts:
        problems.append(f"minSdk {mins} > targetSdk {ts}")
    # Play requires 16 KB alignment for apps targeting Android 15+; NDK r28 does it.
    if ts >= 35 and m_ndk and not (m_ndk.group(1).startswith("28") or
                                   m_ndk.group(1).startswith("27")):
        problems.append(f"targetSdk {ts} needs a 16 KB-aligned toolchain (NDK r27/r28), "
                        f"found {m_ndk.group(1)}")

m_agp = re.search(r'id\("com\.android\.application"\)\s*version\s*"([\d.]+)"', root)
if m_agp:
    major, minor = (int(x) for x in m_agp.group(1).split(".")[:2])
    if (major, minor) < (8, 5):
        problems.append(f"AGP {m_agp.group(1)} is older than 8.5.1, which 16 KB page "
                        "alignment requires")
else:
    problems.append("could not read the AGP version from android/build.gradle.kts")

if m_cmake and not m_cmake.group(1).startswith("3.22"):
    notes.append(f"externalNativeBuild declares CMake {m_cmake.group(1)}; the SDK "
                 "package installed in CI must match")

# ------------------------------------------------------- Kotlin semantic checks
# These are the bugs that a compiler *would* catch, written as checks because
# there is no JDK on the development machine. Each one was a real defect found by
# reading the code line by line.
KOTLIN_FILES = []
for base in KT_DIRS:
    for dirpath, _, files in os.walk(base):
        for f in files:
            if f.endswith(".kt"):
                KOTLIN_FILES.append(os.path.join(dirpath, f))

def find(pattern: str, flags=0):
    import re as _re
    hits = []
    for path in KOTLIN_FILES:
        src = read(path)
        for m in _re.finditer(pattern, src, flags):
            line = src[:m.start()].count("\n") + 1
            hits.append((os.path.relpath(path, ROOT), line, m.group(0)))
    return hits

# 1. ExifInterface: only androidx.exifinterface.media has rotationDegrees.
if any(i.startswith("android.media.ExifInterface") for i in imports):
    problems.append("android.media.ExifInterface has no rotationDegrees; use "
                    "androidx.exifinterface.media.ExifInterface")
if find(r"\.rotationDegrees") and not any(
        i == "androidx.exifinterface.media.ExifInterface" for i in imports):
    problems.append("rotationDegrees used without importing androidx.exifinterface")

# 2. A member the UI assigns must exist on the class it assigns it on.
surface = read(os.path.join(ROOT, "android/app/src/main/java/ai/shadowvision/core/MaskSurface.kt"))
all_src = "\n".join(read(p) for p in KOTLIN_FILES)
for prop in ("zoom", "erasing", "brushRadius"):
    assigned = (f"surface?.{prop} =" in all_src or f"s.{prop} =" in all_src
                or f".{prop} = " in all_src)
    if not assigned:
        continue
    declared_anywhere = f"var {prop}" in surface or re.search(
        rf"var\s+{prop}\b", all_src)
    if not re.search(rf"var\s+{prop}\b", surface):
        problems.append(f"the UI assigns `{prop}` on MaskSurface but the class declares "
                        f"no such property (it has " +
                        ", ".join(re.findall(r"var\s+(\w+)", surface)) + ")")

# 3. Calls into MaskSurface must match its declared functions.
declared = set(re.findall(r"fun ([a-zA-Z]\w*)\(", surface))
for path in KOTLIN_FILES:
    src = read(path)
    if "ai.shadowvision.core.MaskSurface" not in src and "MaskSurface" not in src:
        continue
    for m in re.finditer(r"surface\??\.([a-zA-Z]\w*)\(", src):
        name = m.group(1)
        if name not in declared and name not in ("also", "apply", "let", "run"):
            problems.append(f"{os.path.relpath(path, ROOT)} calls MaskSurface.{name}() "
                            "which does not exist")

# 4. Signatures: for the project's own cross-file API, a call site must supply
#    between the number of required parameters and the total (Kotlin default
#    arguments are legal, so the upper bound is the hard rule).
#
#    Implemented with a real paren scanner, not a regex: a regex over
#    `name(...)` matches across newlines and reports nonsense (a first attempt
#    produced 30 false positives, including `recycle()` "called with 18 args").
def split_top_level(text: str) -> list[str]:
    """Split on commas that are not nested inside (), [], {} or a string."""
    parts, depth, cur, in_str, str_ch = [], 0, "", False, ""
    for ch in text:
        if in_str:
            cur += ch
            if ch == str_ch:
                in_str = False
            continue
        if ch in "\"'":
            in_str, str_ch = True, ch
            cur += ch
            continue
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    return parts


import re as _re

def _trailing_lambda_start(src: str, close_idx: int) -> bool:
    """True if the '{' matching close_idx sits directly after the call's ')'."""
    depth = 0
    i = close_idx
    while i >= 0:
        if src[i] == "}":
            depth += 1
        elif src[i] == "{":
            depth -= 1
            if depth == 0:
                j = i - 1
                while j >= 0 and src[j] in " \t\r\n":
                    j -= 1
                return j >= 0 and src[j] == ")"
        i -= 1
    return False


def scan_args(src: str, open_idx: int):
    """Return (arg_count, end_index) for the call whose '(' is at open_idx."""
    depth = 0
    args = 0
    in_str = False
    str_ch = ""
    i = open_idx
    started = False
    while i < len(src):
        ch = src[i]
        if in_str:
            if ch == "\\":
                i += 2
                continue
            if ch == str_ch:
                in_str = False
            i += 1
            continue
        if ch in "\"'":
            in_str, str_ch = True, ch
            i += 1
            started = True
            continue
        if ch in "([{":
            depth += 1
            if depth == 1:
                started = False
            i += 1
            continue
        if ch in ")]}":
            depth -= 1
            if depth == 0:
                n = args + (1 if started else 0)
                # f(a) { x -> ... }: the trailing lambda is one more argument.
                j = i + 1
                while j < len(src) and src[j] in " \t\r\n":
                    j += 1
                if j < len(src) and src[j] == "{":
                    n += 1
                return (n, i)
            i += 1
            continue
        if ch == "," and depth == 1:
            args += 1
            started = False
            i += 1
            continue
        # `f(a) { x -> ... }` : the trailing lambda is an argument, not part of the
        # next call. Without this the scanner saw `{ _ -> }` as one argument and
        # then a stray `->`, reporting a 1-argument call to a 2-argument function.
        if ch == "}" and depth == 1 and _trailing_lambda_start(src, i):
            args += 1
            started = False
        if not ch.isspace():
            started = True
        i += 1
    return (None, None)


# Only the project's own API is checked; matching framework methods would just
# rediscover the Android SDK from scratch and be wrong about overloads.
PROJECT_API = {
    "smartSelect", "remove", "export", "onMaskChanged", "onUndoState", "onTap",
    "setTool", "setBrushRadius", "loadInpainter", "loadSegmenter", "inpaint",
    "prepareSegmentation", "tapToMask", "catalogVerify", "pathFor", "verifyFile",
    "offerSelection", "exportMask", "setImage", "clearMask", "canUndo", "canRedo",
    "consumeMessage", "revertToOriginal", "setCompare", "importImage", "setZoom",
}

declarations: dict[str, tuple[int, int, str]] = {}
for path in KOTLIN_FILES:
    src = read(path)
    for m in _re.finditer(r"fun\s+(\w+)\s*\(", src):
        name = m.group(1)
        open_idx = m.end() - 1
        _, end_i = scan_args(src, open_idx)
        if end_i is None:
            continue
        params = src[open_idx + 1:end_i]
        plist = [p.strip() for p in split_top_level(params) if p.strip()]
        total = len(plist)
        required = len([p for p in plist if "=" not in p])
        declarations.setdefault(name, (required, total, os.path.basename(path)))

for path in KOTLIN_FILES:
    src = read(path)
    for m in _re.finditer(r"\.(\w+)\s*\(", src):
        name = m.group(1)
        if name not in PROJECT_API or name not in declarations:
            continue
        required, total, _ = declarations[name]
        open_idx = m.end() - 1
        n, _end = scan_args(src, open_idx)
        if n is None:
            continue
        if n < required or n > total:
            line = src[:m.start()].count("\n") + 1
            problems.append(
                f"{os.path.relpath(path, ROOT)}:{line} calls {name}() with {n} args "
                f"but it accepts {required}..{total}")

# 5. Bitmap copies assigned to state must be recycled somewhere, or a full-size
#    bitmap leaks per operation.
vm = read(os.path.join(ROOT, "android/app/src/main/java/ai/shadowvision/studio/StudioViewModel.kt"))
if "originalForCompare = bitmap.copy" in vm and ".recycle()" not in vm:
    problems.append("StudioViewModel stores bitmap copies but never recycles them")

# 6. Unit mixing: a JNI helper that already returns MB must not be divided again.
engine = read(os.path.join(ROOT, "android/app/src/main/java/ai/shadowvision/core/SvEngine.kt"))
if re.search(r"nativeRssMb\(\)\s*/\s*1024", engine):
    problems.append("nativeRssMb already returns MB; dividing by 1024 again makes the "
                    "console RSS gauge read 0")
jni_src = read(os.path.join(ROOT, "android/app/src/main/cpp/jni_bridge.cpp"))
if "kb / 1024.0" not in jni_src and "kb > 0 ? static_cast<jdouble>(kb) / 1024.0" not in jni_src:
    problems.append("could not confirm nativeRssMb returns MB (JNI conversion missing)")

# ---------------------------------------------------------------- JNI class
jni = read(os.path.join(ROOT, "android/app/src/main/cpp/jni_bridge.cpp"))
m_class = re.search(r'kClass\s*=\s*"([^"]+)"', jni)
if m_class:
    cls = m_class.group(1).replace("/", ".")
    expected_path = os.path.join(ROOT, "android/app/src/main/java",
                                 *m_class.group(1).split("/")) + ".kt"
    if not os.path.isfile(expected_path):
        problems.append(f"JNI registers natives on {cls} but {expected_path} does not exist")

if problems:
    print("Android build audit failed:", file=sys.stderr)
    for p in problems:
        print("  - " + p, file=sys.stderr)
    sys.exit(1)

for n in notes:
    print("  note: " + n)
print(f"  Android build audit OK ({len(imports)} imports checked, "
      f"AGP {m_agp.group(1) if m_agp else '?'}, "
      f"NDK {m_ndk.group(1) if m_ndk else '?'})")
