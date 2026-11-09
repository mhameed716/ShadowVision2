#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Create the release keystore, once, and print what to export.
#
# The debug key is not a release key: it is public, it expires, and an APK signed
# with it cannot be updated by a properly signed build later. Losing a release key
# means the app can never be updated under the same applicationId, so this writes
# the file, prints the fingerprints, and tells you to back it up — twice.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-${ROOT}/keys/shadowvision-release.jks}"
ALIAS="${SV_KEY_ALIAS:-shadowvision}"
DNAME="${SV_KEY_DNAME:-CN=ShadowVision, OU=Release, O=ShadowVision, L=Frankfurt, C=DE}"
VALIDITY="${SV_KEY_VALIDITY:-10000}"

if [ -f "${OUT}" ]; then
  echo "refusing to overwrite ${OUT}" >&2
  echo "an existing release key is irreplaceable; move it instead" >&2
  exit 1
fi

command -v keytool >/dev/null || { echo "keytool not found (install a JDK)" >&2; exit 1; }

mkdir -p "$(dirname "${OUT}")"

# A generated password is better than a chosen one and, crucially, the script
# refuses to run with a blank one.
PASS="${SV_KEYSTORE_PASSWORD:-}"
if [ -z "${PASS}" ]; then
  PASS="$(head -c 32 /dev/urandom | base64 | tr -d '/+=' | head -c 32)"
  GENERATED=1
else
  GENERATED=0
fi
[ ${#PASS} -ge 12 ] || { echo "password must be at least 12 characters" >&2; exit 1; }

keytool -genkeypair -v \
  -keystore "${OUT}" \
  -alias "${ALIAS}" \
  -keyalg RSA -keysize 4096 \
  -validity "${VALIDITY}" \
  -storetype PKCS12 \
  -storepass "${PASS}" -keypass "${PASS}" \
  -dname "${DNAME}" >/dev/null

chmod 600 "${OUT}"

echo "created ${OUT}"
echo
keytool -list -v -keystore "${OUT}" -storepass "${PASS}" 2>/dev/null \
  | grep -E "Alias name|Valid from|SHA256:|Signature algorithm" | sed 's/^/  /'
echo
echo "Export these four variables wherever you build the release APK"
echo "(CI: repository secrets; shell: export; never commit them):"
echo
echo "  export SV_KEYSTORE_FILE=${OUT}"
echo "  export SV_KEY_ALIAS=${ALIAS}"
if [ "${GENERATED}" = "1" ]; then
  echo "  export SV_KEYSTORE_PASSWORD=${PASS}"
  echo "  export SV_KEY_PASSWORD=${PASS}"
else
  echo "  export SV_KEYSTORE_PASSWORD=<the one you supplied>"
  echo "  export SV_KEY_PASSWORD=<the one you supplied>"
fi
echo
echo "BACK THIS FILE UP NOW, off this machine, in two places."
echo "If it is lost, no future build can update an installed copy of the app."
