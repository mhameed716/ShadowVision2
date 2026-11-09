// SPDX-License-Identifier: MIT
#include "sv/manifest.h"

#include <cstdio>
#include <cstring>
#include <sstream>

#include "ed25519.h"
#include "sv/sha256.h"

namespace sv {
namespace {

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool iequals(const std::string& a, const char* b) {
    if (a.size() != strlen(b)) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream iss(s);
    while (std::getline(iss, cur, ',')) {
        const std::string t = trim(cur);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

}  // namespace

bool HexDecode(const std::string& hex, std::vector<uint8_t>* out) {
    if (hex.size() % 2 != 0) return false;
    out->clear();
    out->reserve(hex.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nib(hex[i]);
        const int lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

std::string HexEncode(const uint8_t* data, size_t len) {
    static const char* k = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(k[data[i] >> 4]);
        s.push_back(k[data[i] & 0xF]);
    }
    return s;
}

bool IsSafeRelativeFile(const std::string& name) {
    if (name.empty() || name.size() > 128) return false;
    if (name[0] == '/' || name[0] == '~') return false;
    if (name.find("..") != std::string::npos) return false;
    if (name.find(':') != std::string::npos) return false;  // scheme / windows drive
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

const ModelEntry* Catalog::find(const std::string& id) const {
    for (const auto& m : models) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

bool VerifyAndParseCatalog(const std::string& text, const std::string& pubkey_hex, Catalog* out) {
    *out = Catalog{};
    out->verified = false;

    // 1. Split payload / signature. The signature must be the last line.
    const size_t sig_pos = text.rfind("signature=");
    if (sig_pos == std::string::npos) {
        out->reason = "no signature line";
        return false;
    }
    const std::string payload = text.substr(0, sig_pos);
    std::string sig_hex = trim(text.substr(sig_pos + strlen("signature=")));
    // Trailing newline(s) after the signature are tolerated once.
    if (sig_hex.find('\n') != std::string::npos) sig_hex = trim(sig_hex.substr(0, sig_hex.find('\n')));
    out->signature = sig_hex;

    std::vector<uint8_t> sig;
    if (!HexDecode(sig_hex, &sig) || sig.size() != 64) {
        out->reason = "signature is not 64 bytes of hex";
        return false;
    }

    std::vector<uint8_t> pub;
    if (!HexDecode(pubkey_hex, &pub) || pub.size() != 32) {
        out->reason = "public key is not 32 bytes of hex";
        return false;
    }

    // 2. Ed25519 verify over the exact payload bytes (no re-serialisation: we
    //    verify what was signed, we do not rebuild it).
    const int ok = ed25519_verify(reinterpret_cast<const unsigned char*>(sig.data()),
                                  reinterpret_cast<const unsigned char*>(payload.data()),
                                  payload.size(), reinterpret_cast<const unsigned char*>(pub.data()));
    if (ok != 1) {
        out->reason = "signature does not verify against the embedded public key";
        return false;
    }

    // 3. Parse. Strict: unknown keys are ignored, malformed values reject the entry.
    std::istringstream iss(payload);
    std::string line;
    if (!std::getline(iss, line) || trim(line) != "SV-CATALOG-1") {
        out->reason = "missing SV-CATALOG-1 header";
        return false;
    }
    ModelEntry cur;
    bool in_entry = false;
    auto flush = [&]() -> bool {
        if (!in_entry) return true;
        if (cur.id.empty() || cur.file.empty()) {
            out->reason = "entry without id or file";
            return false;
        }
        if (!IsSafeRelativeFile(cur.file)) {
            out->reason = "unsafe file name in entry " + cur.id;
            return false;
        }
        if (cur.sha256.size() != 64) {
            out->reason = "missing sha256 for " + cur.id;
            return false;
        }
        out->models.push_back(cur);
        cur = ModelEntry{};
        return true;
    };
    while (std::getline(iss, line)) {
        const std::string t = trim(line);
        if (t.empty()) continue;
        const size_t eq = t.find('=');
        const std::string key = trim(t.substr(0, eq));
        const std::string val = eq == std::string::npos ? std::string() : trim(t.substr(eq + 1));
        if (key == "key") {
            out->key_id = val;
        } else if (key == "entry") {
            if (!flush()) return false;
            in_entry = true;
        } else if (key == "end") {
            if (!flush()) return false;
            in_entry = false;
        } else if (!in_entry) {
            continue;  // forward-compatible: unknown top-level keys ignored
        } else if (key == "id") {
            cur.id = val;
        } else if (key == "file") {
            cur.file = val;
        } else if (key == "bytes") {
            cur.bytes = strtoll(val.c_str(), nullptr, 10);
        } else if (key == "sha256") {
            cur.sha256 = val;
        } else if (key == "license") {
            cur.license = val;
        } else if (key == "commercial") {
            cur.commercial = iequals(val, "true");
        } else if (key == "input") {
            int w = 0, h = 0, c = 0;
            if (sscanf(val.c_str(), "%dx%dx%d", &w, &h, &c) == 3) {
                cur.input_w = w;
                cur.input_h = h;
            }
        } else if (key == "params") {
            cur.params = val.empty() ? "none" : val;
        } else if (key == "steps") {
            for (const auto& s : split_csv(val)) cur.steps.push_back(atoi(s.c_str()));
        } else if (key == "cfg") {
            for (const auto& s : split_csv(val)) cur.cfg.push_back(static_cast<float>(atof(s.c_str())));
        } else if (key == "seed") {
            cur.seed_supported = iequals(val, "true");
        }
    }
    if (!flush()) return false;
    if (out->models.empty()) {
        out->reason = "catalog is empty";
        return false;
    }
    out->verified = true;
    return true;
}

Status VerifyModelFile(const std::string& models_dir, const ModelEntry& entry, std::string* detail) {
    if (!IsSafeRelativeFile(entry.file)) {
        if (detail) *detail = "unsafe file name";
        return Status::InvalidArgument;
    }
    const std::string path = models_dir + "/" + entry.file;

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (detail) *detail = "missing: " + entry.file;
        return Status::ModelNotLoaded;
    }
    fseek(f, 0, SEEK_END);
    const long long size = ftell(f);
    fclose(f);
    if (entry.bytes > 0 && size != entry.bytes) {
        if (detail) *detail = "size mismatch: expected " + std::to_string(entry.bytes) +
                              " got " + std::to_string(size);
        return Status::InvalidArgument;
    }

    uint8_t digest[32];
    std::string err;
    if (!Sha256::hash_file(path, digest, &err)) {
        if (detail) *detail = err;
        return Status::Internal;
    }
    const std::string got = Sha256::hex(digest);
    if (got != entry.sha256) {
        // Case-insensitive compare, because manifests are hand-edited.
        std::string a = got, b = entry.sha256;
        for (auto& ch : a) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        for (auto& ch : b) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        if (a != b) {
            if (detail) *detail = "sha256 mismatch: expected " + entry.sha256 + " got " + got;
            return Status::InvalidArgument;
        }
    }
    if (detail) *detail = "ok";
    return Status::Ok;
}

}  // namespace sv
