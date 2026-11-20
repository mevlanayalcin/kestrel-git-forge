// Minimal, fast SHA-1 (git object ids, pack trailers).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace kestrel {

struct Sha1 {
  uint32_t h[5];
  uint64_t bits;
  unsigned char block[64];
  size_t used;

  Sha1() { reset(); }

  void reset() {
    h[0] = 0x67452301u;
    h[1] = 0xEFCDAB89u;
    h[2] = 0x98BADCFEu;
    h[3] = 0x10325476u;
    h[4] = 0xC3D2E1F0u;
    bits = 0;
    used = 0;
  }

  static inline uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

  inline void compress(const unsigned char* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
      w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
             (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }

  void update(const void* data, size_t len) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    bits += uint64_t(len) * 8;
    if (used) {
      size_t take = 64 - used;
      if (take > len) take = len;
      std::memcpy(block + used, p, take);
      used += take;
      p += take;
      len -= take;
      if (used == 64) {
        compress(block);
        used = 0;
      }
    }
    while (len >= 64) {
      compress(p);
      p += 64;
      len -= 64;
    }
    if (len) {
      std::memcpy(block + used, p, len);
      used += len;
    }
  }

  void finish(unsigned char out[20]) {
    uint64_t totalBits = bits;
    unsigned char pad = 0x80;
    update(&pad, 1);
    unsigned char zero = 0;
    while (used != 56) update(&zero, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(totalBits >> (56 - i * 8));
    bits = totalBits;  // update() above counted padding; the length write must not
    std::memcpy(block + 56, lenb, 8);
    compress(block);
    used = 0;
    for (int i = 0; i < 5; i++) {
      out[i * 4] = (unsigned char)(h[i] >> 24);
      out[i * 4 + 1] = (unsigned char)(h[i] >> 16);
      out[i * 4 + 2] = (unsigned char)(h[i] >> 8);
      out[i * 4 + 3] = (unsigned char)(h[i]);
    }
  }
};

inline std::string sha1_raw(const void* data, size_t len) {
  Sha1 s;
  s.update(data, len);
  unsigned char out[20];
  s.finish(out);
  return std::string(reinterpret_cast<char*>(out), 20);
}

inline std::string sha1_hex(const void* data, size_t len) {
  static const char* hex = "0123456789abcdef";
  std::string raw = sha1_raw(data, len);
  std::string out(40, '0');
  for (int i = 0; i < 20; i++) {
    unsigned char c = (unsigned char)raw[i];
    out[i * 2] = hex[c >> 4];
    out[i * 2 + 1] = hex[c & 15];
  }
  return out;
}

inline std::string sha1_hex(const std::string& s) { return sha1_hex(s.data(), s.size()); }

}  // namespace kestrel
