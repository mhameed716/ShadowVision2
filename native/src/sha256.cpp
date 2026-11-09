// SPDX-License-Identifier: MIT
#include "sv/sha256.h"

#include <cstdio>
#include <cstring>

namespace sv {
namespace {

constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t load_be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline void store_be(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

}  // namespace

Sha256::Sha256()
    : m_state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
              0x5be0cd19},
      m_bitlen(0),
      m_buffer{},
      m_buffered(0) {}

void Sha256::block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = load_be(p + i * 4);
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + S1 + ch + K[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::update(const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    m_bitlen += static_cast<uint64_t>(len) * 8;
    while (len > 0) {
        const size_t take = std::min(len, sizeof(m_buffer) - m_buffered);
        std::memcpy(m_buffer + m_buffered, p, take);
        m_buffered += take;
        p += take;
        len -= take;
        if (m_buffered == sizeof(m_buffer)) {
            block(m_buffer);
            m_buffered = 0;
        }
    }
}

void Sha256::final(uint8_t out[32]) {
    const uint64_t bits = m_bitlen;
    const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t zero = 0;
    while (m_buffered != 56) update(&zero, 1);
    uint8_t len_be[8];
    for (int i = 0; i < 8; ++i) len_be[i] = uint8_t(bits >> (56 - i * 8));
    // update() would add to m_bitlen again; that is fine, we already captured it.
    update(len_be, 8);
    for (int i = 0; i < 8; ++i) store_be(out + i * 4, m_state[i]);
}

void Sha256::hash(const void* data, size_t len, uint8_t out[32]) {
    Sha256 h;
    h.update(data, len);
    h.final(out);
}

std::string Sha256::hex(const uint8_t digest[32]) {
    static const char* k = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (int i = 0; i < 32; ++i) {
        s.push_back(k[digest[i] >> 4]);
        s.push_back(k[digest[i] & 0xF]);
    }
    return s;
}

bool Sha256::hash_file(const std::string& path, uint8_t out[32], std::string* error) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    Sha256 h;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) h.update(buf, n);
    const bool failed = ferror(f) != 0;
    fclose(f);
    if (failed) {
        if (error) *error = "read error on " + path;
        return false;
    }
    h.final(out);
    return true;
}

}  // namespace sv
