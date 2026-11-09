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
