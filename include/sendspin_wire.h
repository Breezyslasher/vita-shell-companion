/**
 * What the plugin's Sendspin player (plugin/main.cpp, "Sendspin") needs on
 * the wire, with no library under it: a WebSocket's frames read as they
 * come and written masked, as a client writes them (RFC 6455), base64 for
 * the handshake's key, and JSON read in place and written by hand.
 *
 * - Frames are fed in as the socket gives them (wsFeed): any piece, any
 *   size. A message's payload goes to the callback in pieces as it comes,
 *   a fragmented message's continuations included, so that an audio chunk
 *   is never held whole; control frames (close, ping, pong) are held, as
 *   they are 125 bytes at most, and passed on whole.
 * - JSON is read where it lies: ssjMember finds a member of an object, the
 *   rest read a value. Nothing is copied but strings asked for.
 *
 * Plain C with no library calls, in 32 bits and with no division but by
 * 32-bit values, for the plugin, which has neither a C runtime nor 64-bit division; host tests
 * compile it as it is.
 */

#ifndef SC_SENDSPIN_WIRE_H
#define SC_SENDSPIN_WIRE_H

#include <stdint.h>

// ── WebSocket frames ─────────────────────────────────────────────────────────
enum { WS_CONT = 0, WS_TEXT = 1, WS_BINARY = 2, WS_CLOSE = 8, WS_PING = 9, WS_PONG = 10 };

// A piece of a message: op its opcode (WS_TEXT, WS_BINARY, or a control
// frame's), first and last whether it begins and ends the message. A
// message with no payload comes as one empty piece. Not 0 to stop.
typedef int (*WsPiece)(void* ctx, int op, const uint8_t* data, uint32_t n, int first, int last);

typedef struct WsIn {
    uint8_t head[14];
    uint32_t headN, headNeed;  // header bytes held, and wanted so far
    int inPayload;
    int op, fin, masked;
    uint8_t mask[4];
    uint32_t left;      // of this frame's payload
    uint32_t done;      // of it passed on, for the mask
    int msgOp;          // the data message going on (WS_TEXT, WS_BINARY), 0 between them
    int msgStarted;     // a piece of it passed on
    uint8_t ctl[125];   // a control frame's payload, held
    uint32_t ctlN;
} WsIn;

// Errors wsFeed gives.
enum { WS_ERR_BIG = -2, WS_ERR_PROTOCOL = -3 };

static inline void wsInit(WsIn* w) {
    uint8_t* p = (uint8_t*)w;
    for (uint32_t i = 0; i < sizeof(*w); i++) p[i] = 0;
    w->headNeed = 2;
}

static inline int wsIsControl(int op) {
    return op >= 8;
}

// The frame's header, once held whole: 1 when ready for its payload, 0 when
// more of it is wanted, else an error.
static inline int wsHeader(WsIn* w) {
    const uint8_t* h = w->head;
    if (w->headNeed == 2) {
        const uint32_t len7 = h[1] & 0x7F;
        w->headNeed = 2 + (len7 == 126 ? 2 : len7 == 127 ? 8 : 0) + ((h[1] & 0x80) ? 4 : 0);
        if (w->headNeed > 2) return 0;
    }
    w->fin = (h[0] & 0x80) != 0;
    w->op = h[0] & 0x0F;
    w->masked = (h[1] & 0x80) != 0;
    const uint32_t len7 = h[1] & 0x7F;
    uint32_t at = 2;
    if (len7 == 126) {
        w->left = (uint32_t)h[2] << 8 | h[3];
        at = 4;
    } else if (len7 == 127) {
        // Up to 2 GB: what is beyond is no Sendspin message.
        if (h[2] | h[3] | h[4] | h[5] || (h[6] & 0x80)) return WS_ERR_BIG;
        w->left = (uint32_t)h[6] << 24 | (uint32_t)h[7] << 16 | (uint32_t)h[8] << 8 | h[9];
        at = 10;
    } else {
        w->left = len7;
    }
    for (int i = 0; i < 4; i++) w->mask[i] = w->masked ? h[at + i] : 0;
    w->done = 0;
    if (wsIsControl(w->op)) {
        if (!w->fin || w->left > 125) return WS_ERR_PROTOCOL;
        w->ctlN = 0;
    } else if (w->op == WS_CONT) {
        if (!w->msgOp) return WS_ERR_PROTOCOL;
    } else if (w->op == WS_TEXT || w->op == WS_BINARY) {
        if (w->msgOp) return WS_ERR_PROTOCOL;  // a new message inside another
        w->msgOp = w->op;
        w->msgStarted = 0;
    } else {
        return WS_ERR_PROTOCOL;
    }
    w->inPayload = 1;
    return 1;
}

// The frame's end: what it ends passed on, the next header awaited.
static inline int wsFrameEnd(WsIn* w, WsPiece cb, void* ctx) {
    int rc = 0;
    if (wsIsControl(w->op)) {
        rc = cb(ctx, w->op, w->ctl, w->ctlN, 1, 1);
    } else if (w->fin) {
        // A message's last frame: its end, said with an empty piece when
        // the frame had nothing in it to say it with.
        if (w->done == 0) rc = cb(ctx, w->msgOp, w->ctl, 0, !w->msgStarted, 1);
        w->msgOp = 0;
    }
    w->inPayload = 0;
    w->headN = 0;
    w->headNeed = 2;
    return rc;
}

// n bytes from the socket, wherever they fall. 0, the callback's code when
// it stopped, or a WS_ERR_*.
static inline int wsFeed(WsIn* w, uint8_t* data, uint32_t n, WsPiece cb, void* ctx) {
    uint32_t i = 0;
    for (;;) {
        if (!w->inPayload) {
            if (i == n) return 0;
            while (i < n && w->headN < w->headNeed) w->head[w->headN++] = data[i++];
            if (w->headN < w->headNeed) return 0;
            const int h = wsHeader(w);
            if (h < 0) return h;
            if (h == 0) continue;
            if (w->left == 0) {
                const int rc = wsFrameEnd(w, cb, ctx);
                if (rc) return rc;
            }
            continue;
        }
        if (i == n) return 0;
        uint32_t k = n - i < w->left ? n - i : w->left;
        uint8_t* p = data + i;
        if (w->masked)
            for (uint32_t x = 0; x < k; x++) p[x] ^= w->mask[(w->done + x) & 3];
        w->done += k;
        w->left -= k;
        i += k;
        if (wsIsControl(w->op)) {
            for (uint32_t x = 0; x < k; x++) w->ctl[w->ctlN++] = p[x];
        } else {
            const int last = w->left == 0 && w->fin;
            const int rc = cb(ctx, w->msgOp, p, k, !w->msgStarted, last);
            w->msgStarted = 1;
            if (rc) return rc;
        }
        if (w->left == 0) {
            const int rc = wsFrameEnd(w, cb, ctx);
            if (rc) return rc;
        }
    }
}

// A whole frame as a client sends it, masked with mask, into out: its
// length, or 0 when it does not fit.
static inline uint32_t wsFrame(uint8_t* out, uint32_t cap, int op, const uint8_t* data, uint32_t n,
                               const uint8_t mask[4]) {
    uint32_t h = n < 126 ? 2 : n < 65536 ? 4 : 10;
    if (h + 4 + n > cap) return 0;
    out[0] = (uint8_t)(0x80 | op);
    if (n < 126) {
        out[1] = (uint8_t)(0x80 | n);
    } else if (n < 65536) {
        out[1] = 0x80 | 126;
        out[2] = (uint8_t)(n >> 8);
        out[3] = (uint8_t)n;
    } else {
        out[1] = 0x80 | 127;
        for (int i = 0; i < 4; i++) out[2 + i] = 0;
        out[6] = (uint8_t)(n >> 24);
        out[7] = (uint8_t)(n >> 16);
        out[8] = (uint8_t)(n >> 8);
        out[9] = (uint8_t)n;
    }
    for (int i = 0; i < 4; i++) out[h + i] = mask[i];
    h += 4;
    for (uint32_t i = 0; i < n; i++) out[h + i] = data[i] ^ mask[i & 3];
    return h + n;
}

// ── Base64 ───────────────────────────────────────────────────────────────────
// n bytes as base64 into out, ended with 0: its length, or 0 when it does
// not fit.
static inline uint32_t b64Encode(const uint8_t* in, uint32_t n, char* out, uint32_t cap) {
    static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i += 3) {
        if (o + 5 > cap) return 0;
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) |
                           (i + 2 < n ? in[i + 2] : 0);
        out[o++] = kB64[v >> 18];
        out[o++] = kB64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? kB64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? kB64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

// ── JSON, read in place ─────────────────────────────────────────────────────
// Every reader takes a pointer to a value (or to blanks before it) in text
// ended with 0, and is safe on any text: a value cut short, or not one,
// reads as missing.

static inline const char* ssjWs(const char* p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

// Past the string at p (its opening quote), or NULL.
static inline const char* ssjSkipString(const char* p) {
    for (p++; *p; p++) {
        if (*p == '\\') {
            if (!*++p) return 0;
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return 0;
}

// Past the value at p, or NULL when there is none whole there. Nested up to
// 32 deep.
static inline const char* ssjSkip(const char* p) {
    char stack[32];
    int depth = 0;
    p = ssjWs(p);
    if (!p || !*p) return 0;
    do {
        p = ssjWs(p);
        const char c = *p;
        if (!c) return 0;
        if (c == '"') {
            p = ssjSkipString(p);
            if (!p) return 0;
        } else if (c == '{' || c == '[') {
            if (depth == 32) return 0;
            stack[depth++] = c == '{' ? '}' : ']';
            p++;
        } else if (c == '}' || c == ']') {
            if (!depth || stack[depth - 1] != c) return 0;
            depth--;
            p++;
        } else if (c == ',' || c == ':') {
            if (!depth) return 0;
            p++;
        } else {
            // A number, true, false or null: up to what ends it.
            const char* s = p;
            while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' &&
                   *p != '\r' && *p != '\n' && *p != ':' && *p != '"')
                p++;
            if (p == s) return 0;
        }
    } while (depth);
    return p;
}

// Whether the string at p (its opening quote) is key, with nothing escaped.
static inline int ssjKeyIs(const char* p, const char* key) {
    p++;
    for (; *key; key++, p++)
        if (*p != *key) return 0;
    return *p == '"';
}

// The value of the member key of the object at obj, or NULL.
static inline const char* ssjMember(const char* obj, const char* key) {
    const char* p = ssjWs(obj);
    if (!p || *p != '{') return 0;
    p = ssjWs(p + 1);
    if (*p == '}') return 0;
    for (;;) {
        if (*p != '"') return 0;
        const int match = ssjKeyIs(p, key);
        p = ssjSkipString(p);
        if (!p) return 0;
        p = ssjWs(p);
        if (*p != ':') return 0;
        p = ssjWs(p + 1);
        if (match) return p;
        p = ssjSkip(p);
        if (!p) return 0;
        p = ssjWs(p);
        if (*p == '}') return 0;
        if (*p != ',') return 0;
        p = ssjWs(p + 1);
    }
}

// obj.a.b... for keys NULL-ended, or NULL.
static inline const char* ssjPath(const char* obj, const char* const* keys) {
    for (; obj && *keys; keys++) obj = ssjMember(obj, *keys);
    return obj;
}

static inline int ssjHex(char c) {
    return c >= '0' && c <= '9' ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}

static inline int ssjHex4(const char* p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        const int x = ssjHex(p[i]);
        if (x < 0) return -1;
        v = v * 16 + x;
    }
    return v;
}

// The string at p, unescaped, as UTF-8 into out, cut to fit (never in the
// middle of a character). 1 when there is a string there, else 0 with out "".
static inline int ssjString(const char* p, char* out, uint32_t cap) {
    uint32_t o = 0;
    if (cap) out[0] = 0;
    p = ssjWs(p);
    if (!p || *p != '"' || !ssjSkipString(p)) return 0;
    for (p++; *p != '"'; p++) {
        uint32_t c = (unsigned char)*p;
        if (c == '\\') {
            const char e = *++p;
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? 8 : e == 'f' ? 12
                                                                                         : (unsigned char)e;
            if (e == 'u') {
                const int hi = ssjHex4(p + 1);
                c = hi < 0 ? 0xFFFD : (uint32_t)hi;
                if (hi >= 0) p += 4;
                if (c >= 0xD800 && c <= 0xDBFF && p[1] == '\\' && p[2] == 'u') {
                    const int lo = ssjHex4(p + 3);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        c = 0x10000 + ((c - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
                        p += 6;
                    }
                }
                if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
            }
            // Bytes as they are, unescaped ones being UTF-8 already.
            if (e != 'u') {
                if (o + 1 < cap) out[o++] = (char)c;
                continue;
            }
            char b[4];
            uint32_t k;
            if (c < 0x80) {
                b[0] = (char)c;
                k = 1;
            } else if (c < 0x800) {
                b[0] = (char)(0xC0 | (c >> 6));
                b[1] = (char)(0x80 | (c & 0x3F));
                k = 2;
            } else if (c < 0x10000) {
                b[0] = (char)(0xE0 | (c >> 12));
                b[1] = (char)(0x80 | ((c >> 6) & 0x3F));
                b[2] = (char)(0x80 | (c & 0x3F));
                k = 3;
            } else {
                b[0] = (char)(0xF0 | (c >> 18));
                b[1] = (char)(0x80 | ((c >> 12) & 0x3F));
                b[2] = (char)(0x80 | ((c >> 6) & 0x3F));
                b[3] = (char)(0x80 | (c & 0x3F));
                k = 4;
            }
            if (o + k < cap)
                for (uint32_t x = 0; x < k; x++) out[o++] = b[x];
            else
                break;
            continue;
        }
        if (o + 1 < cap) out[o++] = (char)c;
    }
    // Cut: not in the middle of a character.
    if (o + 1 >= cap && cap) {
        while (o > 0 && ((unsigned char)out[o - 1] & 0xC0) == 0x80) o--;
        if (o > 0 && ((unsigned char)out[o - 1] & 0x80)) o--;
    }
    if (cap) out[o] = 0;
    return 1;
}

// The integer at p (a fraction dropped) into *out, kept to 32 bits signed. 1
// when there is a number there.
static inline int ssjInt(const char* p, int32_t* out) {
    p = ssjWs(p);
    if (!p) return 0;
    const int neg = *p == '-';
    if (neg) p++;
    if (*p < '0' || *p > '9') return 0;
    uint32_t v = 0;
    for (; *p >= '0' && *p <= '9'; p++) v = v > 0x7FFFFFFFu / 10u ? 0x7FFFFFFFu : v * 10u + (uint32_t)(*p - '0');
    if (v > 0x7FFFFFFFu) v = 0x7FFFFFFFu;
    *out = neg ? -(int32_t)v : (int32_t)v;
    return 1;
}

// 1 for true, 0 for false, -1 for anything else.
static inline int ssjBool(const char* p) {
    p = ssjWs(p);
    if (!p) return -1;
    if (p[0] == 't' && p[1] == 'r' && p[2] == 'u' && p[3] == 'e') return 1;
    if (p[0] == 'f' && p[1] == 'a' && p[2] == 'l' && p[3] == 's' && p[4] == 'e') return 0;
    return -1;
}

// Whether the array at p holds the string s.
static inline int ssjArrayHas(const char* p, const char* s) {
    p = ssjWs(p);
    if (!p || *p != '[') return 0;
    p = ssjWs(p + 1);
    while (*p && *p != ']') {
        if (*p == '"' && ssjKeyIs(p, s)) return 1;
        p = ssjSkip(p);
        if (!p) return 0;
        p = ssjWs(p);
        if (*p == ',') p = ssjWs(p + 1);
        else break;
    }
    return 0;
}

// ── JSON, written ────────────────────────────────────────────────────────────
// c at out + *n, within cap; *n moves on as though it fitted, so that the
// caller sees afterwards whether all did.
static inline void ssjPutChar(char* out, uint32_t* n, uint32_t cap, char c) {
    if (*n + 1 < cap) {
        out[*n] = c;
        out[*n + 1] = 0;
    }
    (*n)++;
}

// s as a JSON string, quotes and all, as ssjPutChar puts a character.
static inline void ssjPutString(char* out, uint32_t* n, uint32_t cap, const char* s) {
    static const char kHex[] = "0123456789abcdef";
    ssjPutChar(out, n, cap, '"');
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; p++) {
        const unsigned char c = *p;
        if (c == '"' || c == '\\') {
            ssjPutChar(out, n, cap, '\\');
            ssjPutChar(out, n, cap, (char)c);
        } else if (c < 0x20) {
            ssjPutChar(out, n, cap, '\\');
            ssjPutChar(out, n, cap, 'u');
            ssjPutChar(out, n, cap, '0');
            ssjPutChar(out, n, cap, '0');
            ssjPutChar(out, n, cap, kHex[c >> 4]);
            ssjPutChar(out, n, cap, kHex[c & 15]);
        } else {
            ssjPutChar(out, n, cap, (char)c);
        }
    }
    ssjPutChar(out, n, cap, '"');
}

// Text as it is, as ssjPutChar puts a character.
static inline void ssjPut(char* out, uint32_t* n, uint32_t cap, const char* s) {
    for (; s && *s; s++) ssjPutChar(out, n, cap, *s);
}

#endif
