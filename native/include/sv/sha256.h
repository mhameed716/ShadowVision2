// SPDX-License-Identifier: MIT
// SHA-256 (FIPS 180-4), self-contained.
//
// Why hand-written: the manifest verifier must not depend on OpenSSL (not on
// Android), on a 3 MB crypto shim, or on a Java-side implementation that a
// compromised WebView could influence. 120 lines, testable against NIST vectors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sv {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    // Finalises and writes 32 bytes. The object must not be reused after this.
    void final(uint8_t out[32]);

    static void hash(const void* data, size_t len, uint8_t out[32]);
    static std::string hex(const uint8_t digest[32]);

    // Incremental hash of a file, in 64 KB chunks (never maps the whole model:
    // a 200 MB read would evict the page cache we are about to need).
    static bool hash_file(const std::string& path, uint8_t out[32], std::string* error);

private:
    void block(const uint8_t* p);
    uint32_t m_state[8];
    uint64_t m_bitlen;
    uint8_t m_buffer[64];
    size_t m_buffered;
};

}  // namespace sv
