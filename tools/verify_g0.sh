#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Verification gate. Everything here must pass on a clean checkout with no NDK,
# no JDK, no weights and no network access.
#
#   ./tools/verify_g0.sh            # build + tests + catalog + privacy + gate
#   ./tools/verify_g0.sh --asan     # same, under AddressSanitizer/UBSan
#   ./tools/verify_g0.sh --device   # additionally print the on-device steps
#   ./tools/verify_g0.sh --with-ort # also run the real ONNX segmenter against
#                                   # contract-shaped graphs (needs ORT headers +
#                                   # libonnxruntime + python onnx); the only way
#                                   # this path is exercised without a phone
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/native/build"
SAN=OFF
DEVICE=0
WITH_ORT=0
KEYS="${ROOT}/keys"

for arg in "$@"; do
  case "$arg" in
    --asan)   BUILD_DIR="${ROOT}/native/build-asan"; SAN=ON ;;
    --device) DEVICE=1 ;;
    --with-ort) WITH_ORT=1 ;;
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

say "4b/7 ONNX translation units type-check (real ORT headers when available)"
# The ONNX backends (inpainting + segmentation) are only compiled with
# -DSV_WITH_ORT=ON, i.e. never on this machine — which is exactly how a
# signature error survives until a device build. If ORT headers are present we
# type-check them here; otherwise we say so instead of pretending.
ORT_INC="${SV_ORT_INCLUDE:-}"
if [ -z "${ORT_INC}" ] && [ -d /tmp/ortinc/onnxruntime/core/session ]; then
  ORT_INC=/tmp/ortinc/onnxruntime/core/session
fi
if [ -n "${ORT_INC}" ] && [ -f "${ORT_INC}/onnxruntime_cxx_api.h" ]; then
  for f in src/ort_backend.cpp src/ort_segmenter.cpp; do
    "$CXX" -std=c++17 -fsyntax-only -Wall -Wextra -DSV_WITH_ORT=1 \
      -I"${ROOT}/native/include" -I"${ORT_INC}" "${ROOT}/native/${f}"
    echo "  ${f}: OK"
  done
else
  echo "  skipped: ONNX Runtime headers not found."
  echo "  To enable: fetch the include/ tree from the ORT release and set"
  echo "  SV_ORT_INCLUDE=/path/to/onnxruntime/include"
fi

say "4c/7 CI workflow is valid YAML"
# A stray ": " inside an unquoted step name silently invalidates the whole
# workflow file; GitHub then reports "failure" in 0s with no logs, which looks
# exactly like a test failure. Catch it here instead.
if python3 -c "import yaml" >/dev/null 2>&1; then
  python3 "${ROOT}/tools/check_ci_yaml.py"
else
  echo "  skipped: PyYAML not installed"
fi

say "4d/7 Android build wiring (no JDK/SDK needed)"
python3 "${ROOT}/tools/check_jni_binding.py"
# The Gradle module referenced dependencies it never declared (coroutines) and an
# AppCompat activity with no AppCompat on the classpath. A static audit of the
# build files catches that class of break before a 20-minute CI run does.
python3 "${ROOT}/tools/check_android_build.py"

say "5/7 unit tests"
"${BUILD_DIR}/sv_tests"

if [ "${WITH_ORT}" = "1" ]; then
  # --with-ort is a *stricter* gate, so a missing prerequisite is a failure,
  # not a silent skip: coverage that quietly disappears is coverage you do not
  # have.
  python3 "${ROOT}/tools/ort_env.py" --check || {
    echo "  --with-ort requires ONNX Runtime; see tools/ort_env.py --help"; exit 1; }
  # The prerequisite check must actually fail closed. Verified, not assumed.
  if SV_ORT_FORCE_MISSING=1 python3 "${ROOT}/tools/ort_env.py" --check >/dev/null 2>&1; then
    echo "  FAIL: ort_env.py --check passed with ORT forced missing"; exit 1
  fi
  if SV_ORT_FORCE_MISSING=1 python3 "${ROOT}/tools/run_ort_integration.py" --allow-skip >/dev/null 2>&1; then
    echo "  --with-ort: --allow-skip path reported success (expected here, informational)"
  fi
  echo "  prerequisite check fails closed when ORT is absent: OK"

  say "5b/7 ONNX segmenter integration (real graphs, real runtime)"
  python3 "${ROOT}/tools/run_ort_integration.py"
  say "5c/7 ONNX segmenter mutation test (do the assertions have teeth?)"
  python3 "${ROOT}/tools/mutation_test_segmenter.py"
fi

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
# Carry the sanitizer runtime into the ad-hoc binary too, otherwise --asan
# fails at link time with undefined __asan_* symbols from libsv_core.
SAN_FLAGS=""
[ "${SAN}" = "ON" ] && SAN_FLAGS="-fsanitize=address,undefined"
"$CXX" -std=c++17 ${SAN_FLAGS} -I"${ROOT}/native/include" "${BUILD_DIR}/catalog_check.cpp" \
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
