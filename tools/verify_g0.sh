#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Verification gate. Everything here must pass on a clean checkout with no NDK,
# no JDK, no weights and no network access.
#
#   ./tools/verify_g0.sh            # build + tests + catalog + privacy + gate
#   ./tools/verify_g0.sh --asan     # same, under AddressSanitizer/UBSan
#   ./tools/verify_g0.sh --device   # additionally print the on-device steps
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/native/build"
SAN=OFF
DEVICE=0
KEYS="${ROOT}/keys"

for arg in "$@"; do
  case "$arg" in
    --asan)   BUILD_DIR="${ROOT}/native/build-asan"; SAN=ON ;;
    --device) DEVICE=1 ;;
    *) echo "unknown flag: $arg"; exit 2 ;;
  esac
done

say() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

say "1/7 toolchain"
command -v cmake >/dev/null || { echo "cmake missing"; exit 1; }
CXX=$(command -v g++ || command -v clang++) || { echo "no C++ compiler"; exit 1; }
command -v cc >/dev/null || { echo "no C compiler (needed for ed25519)"; exit 1; }
command -v python3 >/dev/null || { echo "python3 missing"; exit 1; }
cmake --version | head -1
"$CXX" --version | head -1

say "2/7 configure"
cmake -S "${ROOT}/native" -B "${BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE=Release -DSV_SANITIZE="${SAN}" -DSV_WITH_ORT=OFF

say "3/7 build"
cmake --build "${BUILD_DIR}" -j "$(nproc 2>/dev/null || echo 2)"

say "4/7 JNI + C ABI type-check (no NDK needed)"
"$CXX" -std=c++17 -fsyntax-only -Wall -Wextra \
  -I"${ROOT}/native/include" -I"${ROOT}/tools/ci-shim" -I"${ROOT}/tools/ci-shim/android" \
  -DSV_WITH_ORT=0 "${ROOT}/android/app/src/main/cpp/jni_bridge.cpp"
"$CXX" -std=c++17 -fsyntax-only -Wall -Wextra \
  -I"${ROOT}/native/include" -I"${ROOT}/tools/ci-shim" -I"${ROOT}/tools/ci-shim/android" \
  -DSV_WITH_ORT=0 "${ROOT}/native/src/sv_c_api.cpp"
echo "jni_bridge.cpp + sv_c_api.cpp: OK"

say "5/7 unit tests"
"${BUILD_DIR}/sv_tests"

say "6/7 signed catalog (Ed25519 end-to-end, exactly as the app verifies)"
if [ ! -f "${KEYS}/dev.key" ]; then
  mkdir -p "${KEYS}"
  python3 "${ROOT}/tools/sign_catalog.py" keygen --key-id dev-0001 --out "${KEYS}" >/dev/null
  echo "generated a throwaway dev key (keys/ is git-ignored, never committed)"
fi
# Deterministic fixture files so the manifest has real sizes and hashes. These
# are NOT models; they exist so CI can exercise the verifier end to end.
python3 - <<'PY'
import os
os.makedirs("models", exist_ok=True)
for name, size in [("lama_fp32.onnx", 4096), ("mobile_sam_encoder.onnx", 2048),
                   ("mobile_sam_decoder.onnx", 1024)]:
    path = os.path.join("models", name)
    if not os.path.exists(path):
        with open(path, "wb") as f:
            f.write(bytes((i * 37 + 11) % 256 for i in range(size)))
PY
python3 "${ROOT}/tools/sign_catalog.py" sign --catalog "${ROOT}/models/catalog.txt" \
        --key "${KEYS}/dev.key" --models "${ROOT}/models" >/dev/null
python3 "${ROOT}/tools/sign_catalog.py" verify --catalog "${ROOT}/models/catalog.signed.txt" \
        --pub "${KEYS}/dev.pub" | tail -1
mkdir -p "${ROOT}/android/app/src/main/assets"
cp "${ROOT}/models/catalog.signed.txt" "${ROOT}/android/app/src/main/assets/catalog.signed.txt"

# The native verifier must accept the same bytes the Python signer produced.
cat > "${BUILD_DIR}/catalog_check.cpp" <<'CPP'
#include "sv/manifest.h"
#include <cstdio>
#include <fstream>
#include <sstream>
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::ifstream f(argv[1]);
    std::stringstream ss; ss << f.rdbuf();
    std::ifstream k(argv[2]);
    std::string pub; k >> pub;
    sv::Catalog cat;
    const bool ok = sv::VerifyAndParseCatalog(ss.str(), pub, &cat);
    printf("native verify: %s (%zu entries) %s\n", ok ? "OK" : "FAILED", cat.models.size(),
           cat.reason.c_str());
    return ok ? 0 : 1;
}
CPP
"$CXX" -std=c++17 -I"${ROOT}/native/include" "${BUILD_DIR}/catalog_check.cpp" \
  "${BUILD_DIR}/libsv_core.a" "${BUILD_DIR}/libsv_ed25519.a" -o "${BUILD_DIR}/catalog_check"
"${BUILD_DIR}/catalog_check" "${ROOT}/models/catalog.signed.txt" "${KEYS}/dev.pub"

say "7/7 privacy guardrails + gate report"
"${ROOT}/tools/check_manifest.sh"
"${BUILD_DIR}/sv_g0" --gen-test "${BUILD_DIR}" >/dev/null
"${BUILD_DIR}/sv_g0" --image "${BUILD_DIR}/test.ppm" --mask "${BUILD_DIR}/test.pgm" \
                     --backend synthetic --repeat 3 --out "${BUILD_DIR}/out.ppm"
"${BUILD_DIR}/sv_g0" --image "${BUILD_DIR}/test.ppm" --mask "${BUILD_DIR}/test.pgm" \
                     --backend synthetic --repeat 1 --emulate-ns 120 --json \
                     > "${BUILD_DIR}/report.json"

if [ "${DEVICE}" = "1" ]; then
  say "device"
  command -v adb >/dev/null || { echo "adb missing"; exit 1; }
  adb shell getprop ro.product.cpu.abi
  adb shell getprop ro.soc.model
  echo "then: cd android && ./gradlew :app:assembleDebug && adb install -r app/build/outputs/apk/debug/app-debug.apk"
fi

say "DONE — verified"
