/**
 * DEFLATE (RFC 1951) decoded as it is read, for the plugin's VPK installer:
 * a ZIP's entries, each stored or deflated. Input comes through a read
 * callback a buffer at a time, output goes out through a write callback each
 * time the 32 KB window fills, with its CRC-32 kept as it goes, so that no
 * file is held whole. Decoding follows Mark Adler's puff (zlib's
 * contrib/puff): canonical Huffman codes, counts and symbols; codes of up to
 * SCI_FAST bits, nearly all of them, are looked up in a table at once, and
 * longer ones decoded a bit at a time as puff does. Bit-at-a-time alone
 * unpacked a 12.7 MB VPK in 42.5 s on a Vita.
 *
 * Plain C with no library calls, in 32 bits and with no division, for the
 * plugin; host tests compile it as it is. The caller gives the memory
 * (ScInflate, its window and its input buffer), so that nothing is held
 * between installs.
 */

#ifndef SC_INFLATE_H
#define SC_INFLATE_H

#include <stdint.h>

#define SCI_WINDOW 32768u
#define SCI_FAST 9  // bits looked up at once

// Bytes into buf, up to cap: how many, 0 at the input's end, < 0 for an error.
typedef int (*SciRead)(void* ctx, uint8_t* buf, uint32_t cap);
// n bytes out: 0, or < 0 to stop with that error.
typedef int (*SciWrite)(void* ctx, const uint8_t* data, uint32_t n);

enum {
    SCI_ERR_INPUT = -1001,     // the input ended, or its read failed
    SCI_ERR_DATA = -1002,      // not valid DEFLATE
    SCI_ERR_DISTANCE = -1003,  // a distance back past the start
};

typedef struct SciHuff {
    uint16_t count[16];  // codes of each length
    uint16_t* symbol;    // symbols by code
    uint16_t* fast;      // 1 << SCI_FAST entries: symbol | length << 9, 0 for a longer code
} SciHuff;

typedef struct ScInflate {
    uint8_t* window;  // SCI_WINDOW bytes, the caller's
    uint8_t* in;      // the input buffer, the caller's
    uint32_t inCap, inPos, inLen;
    SciRead read;
    void* readCtx;
    SciWrite write;
    void* writeCtx;
    uint32_t bits;
    int bitCount;
    uint32_t pos;       // in the window
    uint32_t flushed;   // of the window, written out
    uint32_t total;     // bytes out
    uint32_t crc;       // their CRC-32, as it stands (sciCrcEnd for the final value)
    int err;
    uint32_t crcTable[256];
    uint16_t lenSym[288], distSym[30];
    uint16_t lenFast[1 << SCI_FAST], distFast[1 << SCI_FAST];
} ScInflate;

static inline void sciCrcInit(ScInflate* z) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        z->crcTable[i] = c;
    }
    z->crc = 0xFFFFFFFFu;
}

static inline void sciCrc(ScInflate* z, const uint8_t* p, uint32_t n) {
    uint32_t c = z->crc;
    for (uint32_t i = 0; i < n; i++) c = z->crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    z->crc = c;
}

static inline uint32_t sciCrcEnd(const ScInflate* z) {
    return z->crc ^ 0xFFFFFFFFu;
}

// Set up for a stream: the memory, the callbacks; the CRC table made.
static inline void sciInit(ScInflate* z, uint8_t* window, uint8_t* in, uint32_t inCap, SciRead read,
                           void* readCtx, SciWrite write, void* writeCtx) {
    z->window = window;
    z->in = in;
    z->inCap = inCap;
    z->inPos = z->inLen = 0;
    z->read = read;
    z->readCtx = readCtx;
    z->write = write;
    z->writeCtx = writeCtx;
    z->bits = 0;
    z->bitCount = 0;
    z->pos = z->flushed = z->total = 0;
    z->err = 0;
    sciCrcInit(z);
}

// The window's bytes not yet written, written.
static inline int sciFlush(ScInflate* z) {
    if (z->pos > z->flushed) {
        const uint32_t n = z->pos - z->flushed;
        sciCrc(z, z->window + z->flushed, n);
        const int rc = z->write(z->writeCtx, z->window + z->flushed, n);
        if (rc < 0) return z->err = rc;
    }
    z->flushed = z->pos;
    if (z->pos == SCI_WINDOW) z->pos = z->flushed = 0;
    return 0;
}

static inline int sciPut(ScInflate* z, uint8_t b) {
    z->window[z->pos++] = b;
    z->total++;
    return z->pos == SCI_WINDOW ? sciFlush(z) : 0;
}

static inline int sciByte(ScInflate* z) {
    if (z->inPos == z->inLen) {
        const int got = z->read(z->readCtx, z->in, z->inCap);
        if (got <= 0) {
            z->err = SCI_ERR_INPUT;
            return -1;
        }
        z->inPos = 0;
        z->inLen = (uint32_t)got;
    }
    return z->in[z->inPos++];
}

// The bit buffer topped up to at least 24 bits while there is input, with
// no error at its end: for a look ahead, which may want fewer.
static inline void sciFill(ScInflate* z) {
    while (z->bitCount <= 24) {
        if (z->inPos == z->inLen) {
            const int got = z->read(z->readCtx, z->in, z->inCap);
            if (got <= 0) return;
            z->inPos = 0;
            z->inLen = (uint32_t)got;
        }
        z->bits |= (uint32_t)z->in[z->inPos++] << z->bitCount;
        z->bitCount += 8;
    }
}

// n bits (up to 16), least significant first. -1 at an error.
static inline int sciBits(ScInflate* z, int n) {
    while (z->bitCount < n) {
        const int b = sciByte(z);
        if (b < 0) return -1;
        z->bits |= (uint32_t)b << z->bitCount;
        z->bitCount += 8;
    }
    const uint32_t v = z->bits & ((1u << n) - 1);
    z->bits >>= n;
    z->bitCount -= n;
    return (int)v;
}

// A symbol by h: from its table when its code is short, else a bit at a
// time. -1 at an error.
static inline int sciDecode(ScInflate* z, const SciHuff* h) {
    if (z->bitCount < SCI_FAST) sciFill(z);
    const uint32_t e = h->fast[z->bits & ((1u << SCI_FAST) - 1)];
    const int len = (int)(e >> 9);
    if (e && len <= z->bitCount) {
        z->bits >>= len;
        z->bitCount -= len;
        return (int)(e & 0x1FF);
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        const int b = sciBits(z, 1);
        if (b < 0) return -1;
        code |= b;
        const int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    z->err = SCI_ERR_DATA;
    return -1;
}

// h made from n code lengths: 0 when complete, > 0 when incomplete (allowed
// for a single distance code), < 0 when over-subscribed.
static inline int sciBuild(SciHuff* h, const uint8_t* length, int n) {
    uint16_t offs[16];
    for (int len = 0; len < 16; len++) h->count[len] = 0;
    for (int i = 0; i < (1 << SCI_FAST); i++) h->fast[i] = 0;
    for (int s = 0; s < n; s++) h->count[length[s]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int s = 0; s < n; s++)
        if (length[s]) h->symbol[offs[length[s]]++] = (uint16_t)s;
    // The table: each short code, its bits reversed as they come, and every
    // entry that begins with them.
    if (left < 0) return left;
    int code = 0, k = 0;
    for (int len = 1; len <= SCI_FAST; len++) {
        for (int c = 0; c < h->count[len]; c++, k++, code++) {
            int rev = 0;
            for (int b = 0; b < len; b++) rev |= ((code >> b) & 1) << (len - 1 - b);
            for (int i = rev; i < (1 << SCI_FAST); i += 1 << len)
                h->fast[i] = (uint16_t)(h->symbol[k] | len << 9);
        }
        code <<= 1;
    }
    return left;
}

static const uint16_t kSciLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10,  11,  13,  15,  17,  19,  23, 27,
                                         31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t kSciLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                         2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t kSciDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                          33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                          1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
static const uint8_t kSciDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                          6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// A block's codes, by lencode and distcode, up to its end symbol.
static inline int sciCodes(ScInflate* z, const SciHuff* lencode, const SciHuff* distcode) {
    for (;;) {
        int sym = sciDecode(z, lencode);
        if (sym < 0) return z->err ? z->err : SCI_ERR_DATA;
        if (sym < 256) {
            if (sciPut(z, (uint8_t)sym) < 0) return z->err;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return z->err = SCI_ERR_DATA;
            const int extra = sciBits(z, kSciLenExtra[sym]);
            if (extra < 0) return z->err;
            const uint32_t len = kSciLenBase[sym] + (uint32_t)extra;
            const int ds = sciDecode(z, distcode);
            if (ds < 0) return z->err ? z->err : SCI_ERR_DATA;
            if (ds >= 30) return z->err = SCI_ERR_DATA;
            const int dextra = sciBits(z, kSciDistExtra[ds]);
            if (dextra < 0) return z->err;
            const uint32_t dist = kSciDistBase[ds] + (uint32_t)dextra;
            if (dist > z->total || dist > SCI_WINDOW) return z->err = SCI_ERR_DISTANCE;
            for (uint32_t i = 0; i < len; i++) {
                const uint32_t from = (z->pos + SCI_WINDOW - dist) & (SCI_WINDOW - 1);
                if (sciPut(z, z->window[from]) < 0) return z->err;
            }
        }
    }
}

// The next byte of a stored block: from the bit buffer while it holds whole
// bytes (read ahead for the table), else from the input.
static inline int sciStoredByte(ScInflate* z) {
    if (z->bitCount >= 8) {
        const int b = (int)(z->bits & 0xFF);
        z->bits >>= 8;
        z->bitCount -= 8;
        return b;
    }
    return sciByte(z);
}

static inline int sciStored(ScInflate* z) {
    z->bits >>= z->bitCount & 7;  // to the byte
    z->bitCount -= z->bitCount & 7;
    int b[4];
    for (int i = 0; i < 4; i++)
        if ((b[i] = sciStoredByte(z)) < 0) return z->err;
    const uint32_t len = (uint32_t)b[0] | (uint32_t)b[1] << 8;
    const uint32_t nlen = (uint32_t)b[2] | (uint32_t)b[3] << 8;
    if (len != (~nlen & 0xFFFF)) return z->err = SCI_ERR_DATA;
    for (uint32_t i = 0; i < len; i++) {
        const int c = sciStoredByte(z);
        if (c < 0) return z->err;
        if (sciPut(z, (uint8_t)c) < 0) return z->err;
    }
    return 0;
}

static inline int sciFixed(ScInflate* z) {
    uint8_t lengths[288 + 30];
    SciHuff lencode, distcode;
    lencode.symbol = z->lenSym;
    distcode.symbol = z->distSym;
    lencode.fast = z->lenFast;
    distcode.fast = z->distFast;
    int s = 0;
    for (; s < 144; s++) lengths[s] = 8;
    for (; s < 256; s++) lengths[s] = 9;
    for (; s < 280; s++) lengths[s] = 7;
    for (; s < 288; s++) lengths[s] = 8;
    sciBuild(&lencode, lengths, 288);
    for (s = 0; s < 30; s++) lengths[s] = 5;
    sciBuild(&distcode, lengths, 30);
    return sciCodes(z, &lencode, &distcode);
}

static inline int sciDynamic(ScInflate* z) {
    static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t lengths[288 + 32];
    SciHuff lencode, distcode;
    lencode.symbol = z->lenSym;
    distcode.symbol = z->distSym;
    lencode.fast = z->lenFast;
    distcode.fast = z->distFast;
    const int nlen = sciBits(z, 5), ndist = sciBits(z, 5), ncode = sciBits(z, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return z->err;
    const int nl = nlen + 257, nd = ndist + 1, nc = ncode + 4;
    if (nl > 286 || nd > 30) return z->err = SCI_ERR_DATA;
    int i = 0;
    for (; i < nc; i++) {
        const int b = sciBits(z, 3);
        if (b < 0) return z->err;
        lengths[kOrder[i]] = (uint8_t)b;
    }
    for (; i < 19; i++) lengths[kOrder[i]] = 0;
    if (sciBuild(&lencode, lengths, 19) != 0) return z->err = SCI_ERR_DATA;
    for (i = 0; i < nl + nd;) {
        int sym = sciDecode(z, &lencode);
        if (sym < 0) return z->err ? z->err : SCI_ERR_DATA;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        int len = 0, rep;
        if (sym == 16) {
            if (i == 0) return z->err = SCI_ERR_DATA;
            len = lengths[i - 1];
            rep = sciBits(z, 2);
            if (rep < 0) return z->err;
            rep += 3;
        } else if (sym == 17) {
            rep = sciBits(z, 3);
            if (rep < 0) return z->err;
            rep += 3;
        } else {
            rep = sciBits(z, 7);
            if (rep < 0) return z->err;
            rep += 11;
        }
        if (i + rep > nl + nd) return z->err = SCI_ERR_DATA;
        while (rep--) lengths[i++] = (uint8_t)len;
    }
    if (lengths[256] == 0) return z->err = SCI_ERR_DATA;
    int err = sciBuild(&lencode, lengths, nl);
    if (err < 0 || (err > 0 && nl - lencode.count[0] != 1)) return z->err = SCI_ERR_DATA;
    err = sciBuild(&distcode, lengths + nl, nd);
    if (err < 0 || (err > 0 && nd - distcode.count[0] != 1)) return z->err = SCI_ERR_DATA;
    return sciCodes(z, &lencode, &distcode);
}

// The whole stream decoded and written: 0, or an SCI_ERR_* or the write's
// error. z->total and sciCrcEnd(z) then say what came out.
static inline int sciInflate(ScInflate* z) {
    int last;
    do {
        last = sciBits(z, 1);
        const int type = sciBits(z, 2);
        if (last < 0 || type < 0) return z->err;
        const int rc = type == 0 ? sciStored(z) : type == 1 ? sciFixed(z) : type == 2 ? sciDynamic(z)
                                                                                        : (z->err = SCI_ERR_DATA);
        if (rc < 0) return rc;
    } while (!last);
    return sciFlush(z);
}

// A stored entry's n bytes copied through, with the CRC kept: the same
// callbacks, through the input buffer.
static inline int sciStore(ScInflate* z, uint32_t n) {
    while (n > 0) {
        const int got = z->read(z->readCtx, z->in, z->inCap < n ? z->inCap : n);
        if (got <= 0) return z->err = SCI_ERR_INPUT;
        sciCrc(z, z->in, (uint32_t)got);
        const int rc = z->write(z->writeCtx, z->in, (uint32_t)got);
        if (rc < 0) return z->err = rc;
        z->total += (uint32_t)got;
        n -= (uint32_t)got;
    }
    return 0;
}

#endif
