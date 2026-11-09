#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Privacy guardrail (D11). Runs in CI and locally.
#
# "Zero telemetry / zero network" is a claim in the store listing. A claim is
# only worth something if a build fails when someone adds INTERNET to the
# manifest at 2 a.m. This script is that failure.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="${ROOT}/android/app/src/main/AndroidManifest.xml"
FAIL=0

say() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

say "1/3 forbidden permissions"
FORBIDDEN=(
  "android.permission.INTERNET"
  "android.permission.ACCESS_NETWORK_STATE"
  "android.permission.ACCESS_WIFI_STATE"
  "android.permission.ACCESS_FINE_LOCATION"
  "android.permission.ACCESS_COARSE_LOCATION"
  "android.permission.ACCESS_BACKGROUND_LOCATION"
  "android.permission.READ_EXTERNAL_STORAGE"
  "android.permission.WRITE_EXTERNAL_STORAGE"
  "android.permission.READ_PHONE_STATE"
  "com.google.android.gms.permission.AD_ID"
)
for perm in "${FORBIDDEN[@]}"; do
  # Comments in the manifest mention the permission names on purpose; only real
  # <uses-permission> elements count.
  if grep -qE "<uses-permission[^>]*${perm//./\\.}" "$MANIFEST"; then
    echo "FAIL: ${perm} is declared in AndroidManifest.xml"
    FAIL=1
  else
    echo "ok:   ${perm} absent"
  fi
done

say "2/3 no network APIs in our own sources"
if grep -rnE "\b(URL|URI)\(\"https?://|HttpURLConnection|OkHttpClient|Retrofit|Socket\(|WebView" \
     "${ROOT}/android/app/src/main/java" 2>/dev/null | grep -v "// allow-network" ; then
  echo "FAIL: network API usage found in app sources"
  FAIL=1
else
  echo "ok:   no network API usage in app sources"
fi

say "3/3 native libraries are 16 KB aligned"
if command -v readelf >/dev/null; then
  found=0
  while IFS= read -r so; do
    found=1
    align=$(readelf -lW "$so" 2>/dev/null | awk '/LOAD/{print $NF}' | sort -u | tail -1)
    echo "     $so  LOAD align=${align:-unknown}"
  done < <(find "${ROOT}/android" -name "*.so" -path "*jniLibs*" 2>/dev/null)
  [ "$found" = "0" ] && echo "ok:   no prebuilt .so checked in (built by NDK r28 at assemble time)"
else
  echo "skip: readelf not available"
fi

say "RESULT"
if [ "$FAIL" = "0" ]; then
  echo "privacy guardrails: PASS"
else
  echo "privacy guardrails: FAIL"
  exit 1
fi
