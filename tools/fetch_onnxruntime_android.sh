#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fetch ONNX Runtime for Android and place it where the NDK build expects it:
#
#   android/app/src/main/jniLibs/arm64-v8a/libonnxruntime.so   (runtime)
#   third_party/onnxruntime-android/include/                    (headers)
#
# Both come from the official AAR published by Microsoft for the same version the
# C++ code is compiled against (SV_ORT_VERSION). Using a different version for the
# headers and the .so is how you get a runtime that links and then corrupts memory,
# so this script refuses that combination by construction: there is one version.
#
#   ./tools/fetch_onnxruntime_android.sh                    # pinned version
#   ./tools/fetch_onnxruntime_android.sh --from-aar FILE.aar # offline / already downloaded
#
# The AAR is fetched from Maven Central, not GitHub releases: GitHub release
# tarballs are not reachable from every sandbox, Maven Central is.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${SV_ORT_VERSION:-1.23.2}"
JNI_DIR="${ROOT}/android/app/src/main/jniLibs/arm64-v8a"
INC_DIR="${ROOT}/third_party/onnxruntime-android/include"
# Matches -Psv.ortRoot=third_party/onnxruntime-android: CMake adds <root>/include.
FROM_AAR=""
ABI="${SV_ORT_ABI:-arm64-v8a}"

while [ $# -gt 0 ]; do
  case "$1" in
    --from-aar) FROM_AAR="$2"; shift 2 ;;
    --version)  VERSION="$2"; shift 2 ;;
    --abi)      ABI="$2"; shift 2 ;;
    *) echo "unknown flag: $1" >&2; exit 2 ;;
  esac
done

mkdir -p "${JNI_DIR}" "${INC_DIR}"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

if [ -n "${FROM_AAR}" ]; then
  [ -f "${FROM_AAR}" ] || { echo "no such AAR: ${FROM_AAR}" >&2; exit 1; }
  cp "${FROM_AAR}" "${WORK}/ort.aar"
  echo "using local AAR ${FROM_AAR}"
else
  URL="https://repo1.maven.org/maven2/com/microsoft/onnxruntime/onnxruntime-android/${VERSION}/onnxruntime-android-${VERSION}.aar"
  echo "fetching ${URL}"
  # -f so an HTTP error is not silently unpacked as a "corrupt zip" later.
  curl -fL --retry 3 --retry-delay 5 -o "${WORK}/ort.aar" "${URL}"
fi

EXPECTED=$(( $(stat -c%s "${WORK}/ort.aar") / 1024 ))
[ "${EXPECTED}" -gt 1000 ] || { echo "AAR is suspiciously small; aborting" >&2; exit 1; }

( cd "${WORK}" && unzip -q ort.aar )

SO_SRC="${WORK}/jni/${ABI}/libonnxruntime.so"
[ -f "${SO_SRC}" ] || { echo "AAR does not contain jni/${ABI}/libonnxruntime.so" >&2
                        ls "${WORK}/jni" 2>/dev/null >&2; exit 1; }
cp "${SO_SRC}" "${JNI_DIR}/libonnxruntime.so"

# The AAR ships headers only from 1.16 onwards, inside headers/ — but the set is
# partial. Fall back to the pinned GitHub tag through the API (the only host that
# works from every environment we use) and verify the version string.
if [ -d "${WORK}/headers" ]; then
  if [ -f "${WORK}/headers/onnxruntime_cxx_api.h" ]; then
    cp "${WORK}/headers/"*.h "${INC_DIR}/"
    echo "headers: from the AAR"
  elif [ -d "${WORK}/headers/onnxruntime/core/session" ]; then
    cp "${WORK}/headers/onnxruntime/core/session/"*.h "${INC_DIR}/"
    echo "headers: from the AAR"
  else
    echo "AAR headers have an unexpected layout; falling back to the tag" >&2
  fi
fi
if [ ! -f "${INC_DIR}/onnxruntime_cxx_api.h" ]; then
  echo "headers: fetching the ${VERSION} tag"
  for f in onnxruntime_c_api.h onnxruntime_cxx_api.h onnxruntime_cxx_inline.h \
           onnxruntime_float16.h onnxruntime_ep_c_api.h; do
    curl -fsSL -H "Accept: application/vnd.github.raw" \
      -o "${INC_DIR}/${f}" \
      "https://api.github.com/repos/microsoft/onnxruntime/contents/include/onnxruntime/core/session/${f}?ref=v${VERSION}"
    echo "  ${f} $(wc -c <"${INC_DIR}/${f}") bytes"
  done
fi

# Fail loudly if what we got is not a shared library. `file` is not installed in
# every container, so check the ELF header directly: e_machine 0xB7 = AArch64.
if command -v file >/dev/null 2>&1; then
  file "${JNI_DIR}/libonnxruntime.so" | grep -q "ARM aarch64" || {
    echo "libonnxruntime.so is not an aarch64 shared object:" >&2
    file "${JNI_DIR}/libonnxruntime.so" >&2; exit 1; }
else
  python3 "${ROOT}/tools/check_elf_arch.py" "${JNI_DIR}/libonnxruntime.so" || exit 1
fi
grep -q "ONNXRUNTIME_VERSION_STRING \"${VERSION}\"" "${INC_DIR}/onnxruntime_c_api.h" 2>/dev/null || \
  echo "note: could not confirm the header version string; check it manually"

echo
echo "ONNX Runtime ${VERSION} for ${ABI} is in place:"
echo "  ${JNI_DIR}/libonnxruntime.so"
echo "  ${INC_DIR}/onnxruntime_cxx_api.h"
echo
echo "Now build with -Psv.ortRoot=${ROOT}/third_party/onnxruntime-android"
