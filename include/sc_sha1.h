/**
 * SHA-1 (FIPS 180-1), for the head.bin the plugin makes for a VPK
 * (plugin/head_bin.h), as VitaShell's package installer makes it with its
 * own SHA-1. Plain C, no library calls; host tests compile it as it is.
 */

#ifndef SC_SHA1_H
#define SC_SHA1_H

#include <stdint.h>

typedef struct ScSha1 {
    uint32_t h[5];
    uint32_t lenLo, lenHi;  // bytes so far
    uint8_t block[64];
    uint32_t used;
} ScSha1;

static inline uint32_t scSha1Rol(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

static inline void scSha1Block(ScSha1* s, const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 |
               p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = scSha1Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
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
        const uint32_t t = scSha1Rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = scSha1Rol(b, 30);
        b = a;
        a = t;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

static inline void scSha1Init(ScSha1* s) {
    s->h[0] = 0x67452301u;
    s->h[1] = 0xEFCDAB89u;
    s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xC3D2E1F0u;
    s->lenLo = s->lenHi = 0;
    s->used = 0;
}

static inline void scSha1Update(ScSha1* s, const uint8_t* p, uint32_t n) {
    const uint32_t lo = s->lenLo + n;
    if (lo < s->lenLo) s->lenHi++;
    s->lenLo = lo;
    for (uint32_t i = 0; i < n; i++) {
        s->block[s->used++] = p[i];
        if (s->used == 64) {
            scSha1Block(s, s->block);
            s->used = 0;
        }
    }
}

static inline void scSha1Final(ScSha1* s, uint8_t out[20]) {
    const uint32_t hi = s->lenHi << 3 | s->lenLo >> 29, lo = s->lenLo << 3;
    const uint8_t one = 0x80, zero = 0;
    scSha1Update(s, &one, 1);
    while (s->used != 56) scSha1Update(s, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 4; i++) {
        len[i] = (uint8_t)(hi >> (24 - 8 * i));
        len[4 + i] = (uint8_t)(lo >> (24 - 8 * i));
    }
    scSha1Update(s, len, 8);
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++) out[4 * i + k] = (uint8_t)(s->h[i] >> (24 - 8 * k));
}

#endif
