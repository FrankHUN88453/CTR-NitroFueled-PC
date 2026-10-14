// PS4 symbol NIDs: base64("+-" alphabet) of the first 8 bytes (reversed) of
// SHA1(name + salt), without padding.
#include <cstring>
#include <unordered_map>

#include "common.h"

namespace {

struct Sha1 {
    u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

    static u32 Rol(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

    void Block(const u8* p) {
        u32 w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (u32)p[i * 4] << 24 | (u32)p[i * 4 + 1] << 16 | (u32)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            u32 f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            u32 t = Rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    void Digest(const u8* data, size_t len, u8 out[20]) {
        size_t full = len / 64 * 64;
        for (size_t i = 0; i < full; i += 64) {
            Block(data + i);
        }
        u8 tail[128] = {};
        size_t rem = len - full;
        memcpy(tail, data + full, rem);
        tail[rem] = 0x80;
        size_t tl = rem + 9 <= 64 ? 64 : 128;
        u64 bits = (u64)len * 8;
        for (int i = 0; i < 8; i++) {
            tail[tl - 1 - i] = (u8)(bits >> (8 * i));
        }
        for (size_t i = 0; i < tl; i += 64) {
            Block(tail + i);
        }
        for (int i = 0; i < 5; i++) {
            out[i * 4] = (u8)(h[i] >> 24);
            out[i * 4 + 1] = (u8)(h[i] >> 16);
            out[i * 4 + 2] = (u8)(h[i] >> 8);
            out[i * 4 + 3] = (u8)h[i];
        }
    }
};

const u8 kSalt[16] = {0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
                      0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30};
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

struct NameEntry {
    const char* nid;
    const char* name;
};
const NameEntry kNames[] = {
#include "gen/nid_names.inc"
};

}  // namespace

std::string NameToNid(const std::string& name) {
    std::string buf = name;
    buf.append((const char*)kSalt, sizeof(kSalt));
    u8 dig[20];
    Sha1{}.Digest((const u8*)buf.data(), buf.size(), dig);
    u8 raw[8];
    for (int i = 0; i < 8; i++) {
        raw[i] = dig[7 - i];
    }
    // 8 bytes -> 11 base64 chars (no padding)
    std::string out;
    u32 acc = 0;
    int bits = 0;
    for (u8 b : raw) {
        acc = (acc << 8) | b;
        bits += 8;
        while (bits >= 6) {
            out += kB64[(acc >> (bits - 6)) & 63];
            bits -= 6;
        }
    }
    if (bits) {
        out += kB64[(acc << (6 - bits)) & 63];
    }
    return out;
}

const char* NidToName(const std::string& nid) {
    static const std::unordered_map<std::string, const char*> map = [] {
        std::unordered_map<std::string, const char*> m;
        for (const auto& e : kNames) {
            m.emplace(e.nid, e.name);
        }
        return m;
    }();
    auto it = map.find(nid);
    return it == map.end() ? nullptr : it->second;
}
