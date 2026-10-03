#include "lz.h"

#include <string.h>

#define HASH_BITS 13
#define MIN_MATCH 4
#define LAST_LITERALS 5   /* the format ends every block with a few literals */
#define MF_LIMIT 12       /* no match may start within the last 12 bytes */

static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint32_t hash4(uint32_t v) { return (v * 2654435761u) >> (32 - HASH_BITS); }

static inline uint8_t *put_len(uint8_t *op, size_t len) {
    while (len >= 255) { *op++ = 255; len -= 255; }
    *op++ = (uint8_t)len;
    return op;
}

size_t sd_lz_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    if (cap < sd_lz_bound(n)) return 0;   /* callers size the buffer with sd_lz_bound() */
    uint8_t *op = dst;
    const uint8_t *ip = src, *anchor = src, *end = src + n;
    if (n > MF_LIMIT + 1) {
        uint32_t table[1 << HASH_BITS];
        memset(table, 0, sizeof table);
        const uint8_t *mflimit = end - MF_LIMIT, *matchlimit = end - LAST_LITERALS;
        ip++;
        while (ip < mflimit) {
            uint32_t seq = rd32(ip), h = hash4(seq);
            const uint8_t *ref = src + table[h];
            table[h] = (uint32_t)(ip - src);
            if (ref >= ip || ip - ref > 65535 || rd32(ref) != seq || ref == ip) { ip++; continue; }
            /* extend the match backwards over pending literals, then forwards */
            while (ip > anchor && ref > src && ip[-1] == ref[-1]) { ip--; ref--; }
            const uint8_t *mp = ip + MIN_MATCH, *rp = ref + MIN_MATCH;
            while (mp < matchlimit && *mp == *rp) { mp++; rp++; }
            size_t lit = (size_t)(ip - anchor), mlen = (size_t)(mp - ip) - MIN_MATCH;
            uint8_t *token = op++;
            if (lit >= 15) { *token = 15 << 4; op = put_len(op, lit - 15); } else *token = (uint8_t)(lit << 4);
            memcpy(op, anchor, lit);
            op += lit;
            uint16_t off = (uint16_t)(ip - ref);
            *op++ = (uint8_t)off;
            *op++ = (uint8_t)(off >> 8);
            if (mlen >= 15) { *token |= 15; op = put_len(op, mlen - 15); } else *token |= (uint8_t)mlen;
            ip = mp;
            anchor = ip;
            if (ip < mflimit) table[hash4(rd32(ip - 2))] = (uint32_t)(ip - 2 - src);
        }
    }
    size_t lit = (size_t)(end - anchor);
    uint8_t *token = op++;
    if (lit >= 15) { *token = 15 << 4; op = put_len(op, lit - 15); } else *token = (uint8_t)(lit << 4);
    memcpy(op, anchor, lit);
    op += lit;
    return (size_t)(op - dst);
}

int sd_lz_decompress(const uint8_t *src, size_t n, uint8_t *dst, size_t raw) {
    const uint8_t *ip = src, *iend = src + n;
    uint8_t *op = dst, *oend = dst + raw;
    while (ip < iend) {
        unsigned token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) {
            unsigned b;
            do { if (ip >= iend) return 0; b = *ip++; lit += b; if (lit > raw) return 0; } while (b == 255);
        }
        if (lit > (size_t)(iend - ip) || lit > (size_t)(oend - op)) return 0;
        memcpy(op, ip, lit);
        ip += lit;
        op += lit;
        if (ip == iend) break;   /* the last sequence has literals only */
        if (iend - ip < 2) return 0;
        size_t off = (size_t)ip[0] | ((size_t)ip[1] << 8);
        ip += 2;
        if (off == 0 || off > (size_t)(op - dst)) return 0;
        size_t mlen = token & 15;
        if (mlen == 15) {
            unsigned b;
            do { if (ip >= iend) return 0; b = *ip++; mlen += b; if (mlen > raw) return 0; } while (b == 255);
        }
        mlen += MIN_MATCH;
        if (mlen > (size_t)(oend - op)) return 0;
        const uint8_t *m = op - off;
        if (off >= mlen) { memcpy(op, m, mlen); op += mlen; }
        else for (size_t i = 0; i < mlen; i++) *op++ = m[i];   /* overlapping copy repeats the pattern */
    }
    return op == oend;
}
