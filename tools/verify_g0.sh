#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Verification gate for the G0 harness. Everything here must pass on a clean
# checkout with no NDK, no JDK, no weights and no network access.
#
#   ./tools/verify_g0.sh            # build + tests + gate report
#   ./tools/verify_g0.sh --asan     # same, under AddressSanitizer/UBSan
#   ./tools/verify_g0.sh --device   # additionally push+run on a connected device
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/native/build"
BUILD_TYPE=Release
DEVICE=0

for arg in "$@"; do
  case "$arg" in
    --asan)   BUILD_DIR="${ROOT}/native/build-asan"; BUILD_TYPE=Asan ;;
    --device) DEVICE=1 ;;
    *) echo "unknown flag: $arg"; exit 2 ;;
  esac
done

say() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

say "1/6 toolchain"
command -v cmake >/dev/null || { echo "cmake missing"; exit 1; }
command -v g++   >/dev/null || command -v clang++ >/dev/null || { echo "no C++ compiler"; exit 1; }
cmake --version | head -1
(c++ --version || g++ --version) | head -1

say "2/6 configure (${BUILD_TYPE})"
SAN=OFF
[ "${BUILD_TYPE}" = "Asan" ] && SAN=ON
cmake -S "${ROOT}/native" -B "${BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE=Release -DSV_SANITIZE="${SAN}" -DSV_WITH_ORT=OFF

say "3/6 build"
cmake --build "${BUILD_DIR}" -j "$(nproc 2>/dev/null || echo 2)"

say "4/6 JNI glue type-check (no NDK needed)"
mkdir -p "${ROOT}/tools/ci-shim/android"
CXX=$(command -v g++ || command -v clang++)
"${CXX}" -std=c++17 -fsyntax-only -Wall -Wextra \
  -I"${ROOT}/native/include" -I"${ROOT}/tools/ci-shim" -I"${ROOT}/tools/ci-shim/android" \
  -DSV_WITH_ORT=0 "${ROOT}/android/app/src/main/cpp/jni_bridge.cpp"
"${CXX}" -std=c++17 -fsyntax-only -Wall -Wextra \
  -I"${ROOT}/native/include" -I"${ROOT}/tools/ci-shim" -I"${ROOT}/tools/ci-shim/android" \
  -DSV_WITH_ORT=0 "${ROOT}/native/src/sv_c_api.cpp"
echo "jni_bridge.cpp + sv_c_api.cpp: OK"

say "5/6 unit tests"
"${BUILD_DIR}/sv_tests"

say "6/6 gate report (synthetic backend, generated 1920x1440 fixture)"
"${BUILD_DIR}/sv_g0" --gen-test "${BUILD_DIR}" >/dev/null
"${BUILD_DIR}/sv_g0" --image "${BUILD_DIR}/test.ppm" --mask "${BUILD_DIR}/test.pgm" \
                     --backend synthetic --repeat 3 --out "${BUILD_DIR}/out.ppm"
"${BUILD_DIR}/sv_g0" --image "${BUILD_DIR}/test.ppm" --mask "${BUILD_DIR}/test.pgm" \
                     --backend synthetic --repeat 1 --emulate-ns 120 --json \
                     > "${BUILD_DIR}/report.json"
echo "json report: ${BUILD_DIR}/report.json"

if [ "${DEVICE}" = "1" ]; then
  say "7/7 device run"
  command -v adb >/dev/null || { echo "adb missing"; exit 1; }
  ABI=$(adb shell getprop ro.product.cpu.abi | tr -d '\r')
  echo "device ABI: ${ABI}"
  echo "(device binaries are produced by the NDK build; run:"
  echo "   ./gradlew :app:assembleDebug && adb install -r app/build/outputs/apk/debug/app-debug.apk)"
fi

say "DONE — G0 harness verified"
