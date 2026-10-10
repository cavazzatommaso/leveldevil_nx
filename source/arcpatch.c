/* arcpatch.c -- hide the on-screen touch buttons (config: touch_buttons = 0).
 *
 * On Android the game always shows its virtual left/right/jump arrows, and
 * the Defold build has no setting to turn them off. They cannot simply be
 * left out: the virtualbuttons GUI also forwards keyboard input to the
 * players and drives the level layout (tried; levels broke). So only the
 * arrow images are made invisible: in the two GUI scenes
 * (resources/gui/virtualbuttons/virtualbuttons.gui and its 2-player
 * variant) every img_* node gets alpha 0. The script, the hitboxes and
 * everything else run exactly as on a phone.
 *
 * In memory only: game.arci (index) + game.arcd (data) are what the engine
 * reads; each scene entry is LZ4-decompressed, rewritten, and appended to
 * the data as a plain entry, and its index slot pointed there. assetmgr.c
 * serves the patched pair. Entries are found by content hash (Level Devil
 * 1.4.1) and must decode to the expected scene; anything unexpected leaves
 * the archive untouched.
 *
 * Plain C, no libnx, so tests/arcpatch_test.c can run it on a desktop.
 *
 * MIT licensed, see LICENSE.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arcpatch.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* ---- XTEA-CTR, as dmCrypt: big-endian words, 64-bit counter from 0 ---- */
static void xtea_ctr(uint8_t *d, size_t n)
{
    static const uint8_t key[16] = "aQj8CScgNP4VsfXK";
    uint32_t k[4];
    uint64_t ctr = 0;
    size_t i;
    int j;

    for (j = 0; j < 4; j++)
        k[j] = be32(key + 4 * j);
    for (i = 0; i < n; i += 8, ctr++) {
        uint32_t v0 = (uint32_t)(ctr >> 32), v1 = (uint32_t)ctr, s = 0;
        uint8_t ks[8];
        int r;
        for (r = 0; r < 32; r++) {
            v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (s + k[s & 3]);
            s += 0x9E3779B9u;
            v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (s + k[(s >> 11) & 3]);
        }
        put_be32(ks, v0);
        put_be32(ks + 4, v1);
        for (j = 0; j < 8 && i + (size_t)j < n; j++)
            d[i + j] ^= ks[j];
    }
}

/* ---- LZ4 block decompression; 0 on any malformed input ---- */
static int lz4(const uint8_t *s, size_t sn, uint8_t *o, size_t on)
{
    size_t i = 0, w = 0;
    while (i < sn) {
        unsigned t = s[i++];
        size_t l = t >> 4, m;
        if (l == 15) {
            unsigned b;
            do {
                if (i >= sn) return 0;
                b = s[i++];
                l += b;
            } while (b == 255);
        }
        if (l > sn - i || l > on - w) return 0;
        memcpy(o + w, s + i, l);
        i += l;
        w += l;
        if (i >= sn) break;                        /* last sequence: literals only */
        if (sn - i < 2) return 0;
        {
            size_t off = s[i] | (size_t)s[i + 1] << 8;
            i += 2;
            m = (t & 15);
            if (m == 15) {
                unsigned b;
                do {
                    if (i >= sn) return 0;
                    b = s[i++];
                    m += b;
                } while (b == 255);
            }
            m += 4;
            if (!off || off > w || m > on - w) return 0;
            while (m--) {
                o[w] = o[w - off];
                w++;
            }
        }
    }
    return w == on;
}

/* ---- protobuf: just enough to rewrite a gui SceneDesc ---- */
static int rd_varint(const uint8_t *p, size_t n, size_t *i, uint64_t *v)
{
    int sh = 0;
    *v = 0;
    while (*i < n && sh < 64) {
        uint8_t c = p[(*i)++];
        *v |= (uint64_t)(c & 0x7f) << sh;
        if (!(c & 0x80))
            return 1;
        sh += 7;
    }
    return 0;
}

static size_t wr_varint(uint8_t *o, uint64_t v)
{
    size_t n = 0;
    do {
        o[n++] = (uint8_t)((v & 0x7f) | (v > 0x7f ? 0x80 : 0));
        v >>= 7;
    } while (v);
    return n;
}

/* One field starting at *i: its number, wire type and (for length-delimited)
 * payload; *i moves past it. 0 on malformed input. */
static int next_field(const uint8_t *p, size_t n, size_t *i, uint32_t *field, uint32_t *wire,
                      const uint8_t **val, size_t *vlen)
{
    uint64_t key, v;
    size_t start;
    if (!rd_varint(p, n, i, &key))
        return 0;
    *field = (uint32_t)(key >> 3);
    *wire = (uint32_t)(key & 7);
    start = *i;
    switch (*wire) {
    case 0: if (!rd_varint(p, n, i, &v)) return 0; break;
    case 1: if (n - *i < 8) return 0; *i += 8; break;
    case 5: if (n - *i < 4) return 0; *i += 4; break;
    case 2:
        if (!rd_varint(p, n, i, &v) || v > n - *i) return 0;
        start = *i;
        *i += (size_t)v;
        break;
    default: return 0;
    }
    *val = p + start;
    *vlen = *i - start;
    return 1;
}

#define SCENE_SCRIPT "/resources/gui/virtualbuttons/virtualbuttons.gui_scriptc"
#define F_SCENE_SCRIPT 1
#define F_SCENE_NODE   6
#define F_NODE_ID      11
#define F_NODE_ALPHA   30

/* Copy a SceneDesc, appending alpha = 0 to every node whose id starts with
 * "img_" (protobuf keeps the last value of a field). Returns the new size
 * and the number of nodes hidden, or 0 if this is not the expected scene. */
static size_t hide_arrows(const uint8_t *in, size_t n, uint8_t *out, int *hidden)
{
    size_t i = 0, w = 0;
    int script_ok = 0;
    *hidden = 0;
    while (i < n) {
        size_t at = i, vlen;
        uint32_t f, wt;
        const uint8_t *v;
        if (!next_field(in, n, &i, &f, &wt, &v, &vlen))
            return 0;
        if (f == F_SCENE_SCRIPT && wt == 2)
            script_ok = vlen == strlen(SCENE_SCRIPT) && !memcmp(v, SCENE_SCRIPT, vlen);
        if (f == F_SCENE_NODE && wt == 2) {
            size_t j = 0, fl;
            uint32_t nf, nw;
            const uint8_t *nv;
            int is_arrow = 0;
            while (j < vlen) {
                if (!next_field(v, vlen, &j, &nf, &nw, &nv, &fl))
                    return 0;
                if (nf == F_NODE_ID && nw == 2 && fl >= 4 && !memcmp(nv, "img_", 4))
                    is_arrow = 1;
            }
            w += wr_varint(out + w, (uint64_t)F_SCENE_NODE << 3 | 2);
            w += wr_varint(out + w, vlen + (is_arrow ? 6 : 0));
            memcpy(out + w, v, vlen);
            w += vlen;
            if (is_arrow) {
                static const uint8_t alpha0[6] = { 0xf5, 0x01, 0, 0, 0, 0 };  /* field 30, float 0 */
                memcpy(out + w, alpha0, 6);
                w += 6;
                (*hidden)++;
            }
            continue;
        }
        memcpy(out + w, in + at, i - at);
        w += i - at;
    }
    return script_ok ? w : 0;
}

typedef struct { const char *hash; int arrows; } Target;

/* Level Devil 1.4.1 (version code 192): the 1-player and 2-player scenes. */
static const Target TARGETS[] = {
    { "fdae10ed0b7e094933db7842115bb491e8be3801", 3 },
    { "f42bab8e2bfb055b26d5c42c73d43dbcf63f5b35", 6 },
};

static int hex_is(const uint8_t *h, size_t hlen, const char *hex)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    if (strlen(hex) != hlen * 2) return 0;
    for (i = 0; i < hlen; i++)
        if (hex[2 * i] != digits[h[i] >> 4] || hex[2 * i + 1] != digits[h[i] & 15])
            return 0;
    return 1;
}

int arcpatch_hide_touch_buttons(const uint8_t *arci, size_t arci_len,
                                const uint8_t *arcd, size_t arcd_len, ArcPatch *out,
                                char *why, size_t whylen)
{
    uint32_t count, eoff, hoff, hlen;
    uint8_t *idx = NULL, *data = NULL;
    size_t data_len = arcd_len;
    int done = 0;
    size_t t;

    memset(out, 0, sizeof(*out));
#define FAIL(msg) do { if (why && whylen) strncpy(why, msg, whylen - 1), why[whylen - 1] = 0; \
                       free(idx); free(data); return 0; } while (0)
    if (arci_len < 32 || be32(arci) != 6) FAIL("unexpected archive index version");
    count = be32(arci + 16);
    eoff = be32(arci + 20);
    hoff = be32(arci + 24);
    hlen = be32(arci + 28);
    if (hlen > 64 || (size_t)hoff + (size_t)count * 64 > arci_len ||
        (size_t)eoff + (size_t)count * 16 > arci_len)
        FAIL("archive index out of range");

    if (!(idx = malloc(arci_len))) FAIL("out of memory");
    memcpy(idx, arci, arci_len);

    for (t = 0; t < sizeof(TARGETS) / sizeof(TARGETS[0]); t++) {
        uint32_t n;
        for (n = 0; n < count; n++) {
            uint8_t *e = idx + eoff + (size_t)n * 16, *plain, *raw, *grown, *scene;
            uint32_t flags, off, size, csize;
            size_t scene_len;
            int hidden;

            if (!hex_is(idx + hoff + (size_t)n * 64, hlen, TARGETS[t].hash))
                continue;
            flags = be32(e);
            off = be32(e + 4);
            size = be32(e + 8);
            csize = be32(e + 12);
            if (csize == 0xFFFFFFFFu || (size_t)off + csize > arcd_len || !size)
                FAIL("target entry is not a compressed entry in range");
            raw = malloc(csize);
            plain = malloc(size);
            if (!raw || !plain) { free(raw); free(plain); FAIL("out of memory"); }
            memcpy(raw, arcd + off, csize);
            if (flags & 0x10000000u)
                xtea_ctr(raw, csize);
            if (!lz4(raw, csize, plain, size)) {
                free(raw); free(plain);
                FAIL("target entry did not decompress");
            }
            free(raw);
            scene = malloc((size_t)size + 256);    /* +6 per arrow, +1 per node length */
            if (!scene) { free(plain); FAIL("out of memory"); }
            scene_len = hide_arrows(plain, size, scene, &hidden);
            free(plain);
            if (!scene_len || hidden != TARGETS[t].arrows) {
                free(scene);
                FAIL("target entry is not the expected virtual buttons scene");
            }

            grown = realloc(data, data_len + scene_len - arcd_len);
            if (!grown) { free(scene); FAIL("out of memory"); }
            data = grown;
            memcpy(data + (data_len - arcd_len), scene, scene_len);
            free(scene);
            put_be32(e, 0);                         /* plain: not compressed, not encrypted */
            put_be32(e + 4, (uint32_t)data_len);
            put_be32(e + 8, (uint32_t)scene_len);
            put_be32(e + 12, 0xFFFFFFFFu);
            data_len += scene_len;
            out->constants += hidden;
            done++;
            break;
        }
    }
    if (done != (int)(sizeof(TARGETS) / sizeof(TARGETS[0])))
        FAIL("virtual buttons scenes not found (different game version?)");

    /* The data the engine sees: the original archive, then the patched entries. */
    {
        size_t extra = data_len - arcd_len;
        uint8_t *full = malloc(data_len);
        if (!full) FAIL("out of memory");
        memcpy(full, arcd, arcd_len);
        memcpy(full + arcd_len, data, extra);
        free(data);
        out->arcd = full;
    }
    out->arci = idx;
    out->arci_len = arci_len;
    out->arcd_len = data_len;
    out->entries = done;
#undef FAIL
    return 1;
}
