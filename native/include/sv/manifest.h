// SPDX-License-Identifier: MIT
// Signed model catalog (docs/decisions-ar.md D10).
//
// The app trusts exactly one thing: a manifest whose Ed25519 signature verifies
// against a public key compiled into the binary. Everything else — file names,
// sha256, licence flags, parameter schema — is data the manifest carries.
//
// Threat model this closes: an attacker who can write to app storage (rooted
// device, backup restore, or a malicious "custom model" download) cannot make
// the app load a model that was not signed by the key holder. A crafted ONNX
// graph is arbitrary memory allocation and, via custom operators, potentially
// more; it must never be reachable from user-controlled storage in release.
//
// Wire format (kept deliberately dumb so it is auditable by eye):
//
//   SV-CATALOG-1
//   key=<key id>
//   entry
//   id=lama.fp32.512
//   file=lama_fp32.onnx
//   bytes=199999999
//   sha256=<64 hex>
//   license=Apache-2.0
//   commercial=true
//   input=512x512x3
//   params=none
//   steps=
//   cfg=
//   seed=false
//   end
//   signature=<128 hex>          <-- last line, over every byte before it
//
// Anything that fails to parse is a rejection, not a warning: a manifest that is
// half-understood is a manifest that is wrong.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sv/common.h"

namespace sv {

struct ModelEntry {
    std::string id;
    std::string file;      // relative to the models directory; no ".."
    int64_t bytes = 0;
    std::string sha256;    // lowercase hex, 64 chars
    std::string license;
    bool commercial = false;
    int input_w = 512;
    int input_h = 512;
    // Parameter schema: the UI renders ONLY these knobs (D4/§3.3). Empty means
    // the model is deterministic and must not show steps/cfg/seed at all.
    std::string params;    // "none" | "lcm" | "sd"
    std::vector<int> steps;
    std::vector<float> cfg;
    bool seed_supported = false;
};

struct Catalog {
    std::string key_id;
    std::vector<ModelEntry> models;
    bool verified = false;
    std::string reason;    // human-readable why verification failed
    std::string signature; // hex, as parsed (verification strips it)

    const ModelEntry* find(const std::string& id) const;
};

// Verifies the Ed25519 signature over the payload (everything before the
// signature line) and then parses entries. `pubkey_hex` is 32 bytes of hex.
bool VerifyAndParseCatalog(const std::string& text, const std::string& pubkey_hex,
                           Catalog* out);

// Checks that the file on disk matches the entry (size + sha256). Deleting a
// mismatching file is the caller's decision, not this function's.
Status VerifyModelFile(const std::string& models_dir, const ModelEntry& entry,
                       std::string* detail);

// Rejects path traversal and absolute paths in a manifest-supplied file name.
bool IsSafeRelativeFile(const std::string& name);

// Hex helpers (also used by the signing tool's tests).
bool HexDecode(const std::string& hex, std::vector<uint8_t>* out);
std::string HexEncode(const uint8_t* data, size_t len);

}  // namespace sv
