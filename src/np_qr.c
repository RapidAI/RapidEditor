#include "np_qr.h"

#include <stdlib.h>
#include <string.h>

/* QR Code Model 2 encoder, byte mode, ECC level M, versions 1..15.
 * Adapted from the well-known public-domain algorithm (Nayuki's QR Code
 * generator) into a self-contained C implementation.
 *
 * Pipeline: choose version -> build data codewords (mode+len+data+pad) ->
 * Reed-Solomon error correction -> interleave (single block) -> place modules
 * with function patterns -> apply best mask -> write format/version info. */

/* ---- Reed-Solomon over GF(256), primitive 0x11D ------------------------ */

static uint8_t gf_mul(uint8_t x, uint8_t y) {
    uint8_t z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (uint8_t)((z << 1) ^ ((z >> 7) * 0x11D));
        z ^= ((y >> i) & 1) ? x : 0;
    }
    return z;
}

/* Generator polynomial coefficients (monic, length degree+1) for `degree`
 * error-correction codewords. */
static void rs_generator(int degree, uint8_t *result) {
    memset(result, 0, (size_t)degree + 1);
    result[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            result[j] = gf_mul(result[j], root);
            if (j + 1 < degree) result[j] ^= result[j + 1];
        }
        root = gf_mul(root, 2);
    }
}

static void rs_remainder(const uint8_t *data, int data_len,
                         const uint8_t *gen, int degree, uint8_t *rem) {
    memset(rem, 0, (size_t)degree);
    for (int i = 0; i < data_len; i++) {
        uint8_t factor = data[i] ^ rem[0];
        memmove(rem, rem + 1, (size_t)(degree - 1));
        rem[degree - 1] = 0;
        for (int j = 0; j < degree; j++) rem[j] ^= gf_mul(gen[j], factor);
    }
}

/* ---- Version tables for ECC level M ------------------------------------ */

/* Total codewords per version (including ECC). */
static const int TOTAL_CODEWORDS[NP_QR_MAX_VERSION + 1] = {
    0, 26, 44, 70, 100, 134, 172, 196, 242, 292, 346, 404, 466, 532, 581, 655
};

/* Error-correction codewords PER BLOCK and number of blocks, level M, v1..15
 * (Nayuki's QR tables, derived from ISO/IEC 18004). Data codewords are split
 * across blocks, each block gets its own RS remainder, then data and ECC are
 * interleaved. */
static const int ECC_PER_BLOCK_M[NP_QR_MAX_VERSION + 1] = {
    0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24
};
static const int NUM_BLOCKS_M[NP_QR_MAX_VERSION + 1] = {
    0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10
};

/* Split `data` (data_cw codewords) into num_blocks, compute ECC per block,
 * then interleave into `out` (total codewords). `raw` holds data_cw bytes,
 * arranged as: all short blocks first, then long blocks (Nayuki convention). */
static void interleave_codewords(const uint8_t *raw, int data_cw,
                                 int num_blocks, int ecc_per_block,
                                 uint8_t *out) {
    int short_blocks = num_blocks - data_cw % num_blocks;
    int short_len = data_cw / num_blocks;
    int long_len = short_len + 1;

    uint8_t ecc[NP_QR_MAX_MODULES];
    const uint8_t *dblocks[16];
    int dlen[16];
    const uint8_t *p = raw;
    for (int i = 0; i < num_blocks; i++) {
        dlen[i] = i < short_blocks ? short_len : long_len;
        dblocks[i] = p;
        p += dlen[i];
    }

    uint8_t gen[32], rem[32];
    rs_generator(ecc_per_block, gen);
    uint8_t eblocks[16][32];
    for (int i = 0; i < num_blocks; i++)
        rs_remainder(dblocks[i], dlen[i], gen, ecc_per_block, eblocks[i]);
    (void)ecc; (void)rem;

    int idx = 0;
    for (int col = 0; col < long_len; col++) {
        for (int i = 0; i < num_blocks; i++) {
            if (col < dlen[i]) out[idx++] = dblocks[i][col];
        }
    }
    for (int col = 0; col < ecc_per_block; col++) {
        for (int i = 0; i < num_blocks; i++) out[idx++] = eblocks[i][col];
    }
}

/* ---- Bit buffer -------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t len; /* bit length */
} bitbuf;

static void bb_append_bits(bitbuf *b, unsigned val, int nbits) {
    for (int i = nbits - 1; i >= 0; i--) {
        size_t byte = b->len >> 3;
        if (byte < b->cap) {
            b->data[byte] |= (uint8_t)(((val >> i) & 1) << (7 - (b->len & 7)));
        }
        b->len++;
    }
}

/* ---- Module matrix helpers --------------------------------------------- */

typedef struct {
    int size;
    uint8_t fn[NP_QR_MAX_MODULES][NP_QR_MAX_MODULES]; /* is function module */
    uint8_t mod[NP_QR_MAX_MODULES][NP_QR_MAX_MODULES];
} matrix;

static void set_function_module(matrix *m, int x, int y, bool dark) {
    if (x < 0 || y < 0 || x >= m->size || y >= m->size) return;
    m->mod[y][x] = dark ? 1 : 0;
    m->fn[y][x] = 1;
}

static void draw_finder(matrix *m, int x, int y) {
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            int dist = adx > ady ? adx : ady;
            bool dark = (dist != 2 && dist != 4);
            set_function_module(m, x + dx, y + dy, dark);
        }
    }
}

static void draw_alignment(matrix *m, int x, int y) {
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            int dist = adx > ady ? adx : ady;
            set_function_module(m, x + dx, y + dy, dist != 1);
        }
    }
}

static void draw_timing(matrix *m) {
    for (int i = 0; i < m->size; i++) {
        set_function_module(m, 6, i, i % 2 == 0);
        set_function_module(m, i, 6, i % 2 == 0);
    }
}

static void draw_format_bits(matrix *m, int mask) {
    /* ECC level M = binary 00. data = (ecc bits << 3) | mask. */
    int data = (0 << 3) | mask;
    int rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = ((data << 10) | rem) ^ 0x5412;

    for (int i = 0; i <= 5; i++) set_function_module(m, 8, i, ((bits >> i) & 1) != 0);
    set_function_module(m, 8, 7, ((bits >> 6) & 1) != 0);
    set_function_module(m, 8, 8, ((bits >> 7) & 1) != 0);
    set_function_module(m, 7, 8, ((bits >> 8) & 1) != 0);
    for (int i = 9; i < 15; i++) set_function_module(m, 14 - i, 8, ((bits >> i) & 1) != 0);

    for (int i = 0; i < 8; i++) set_function_module(m, m->size - 1 - i, 8, ((bits >> i) & 1) != 0);
    for (int i = 8; i < 15; i++) set_function_module(m, 8, m->size - 15 + i, ((bits >> i) & 1) != 0);
    set_function_module(m, 8, m->size - 8, true); /* dark module */
}

static void draw_version_bits(matrix *m, int version) {
    if (version < 7) return;
    int rem = version;
    for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    int bits = (version << 12) | rem;
    for (int i = 0; i < 18; i++) {
        int bit = (bits >> i) & 1;
        int a = m->size - 11 + (i % 3);
        int b = i / 3;
        set_function_module(m, a, b, bit != 0);
        set_function_module(m, b, a, bit != 0);
    }
}

/* Alignment pattern center coordinates per version (from ISO/IEC 18004).
 * Version 1 has none; the finder patterns occupy the (6,6)/(6,size-7)/
 * (size-7,6) corners, which are skipped when placing alignments. */
static const int ALIGN_POS[NP_QR_MAX_VERSION + 1][7] = {
    {0},                    /* 0 unused */
    {0},                    /* 1: none */
    {6, 18},                /* 2 */
    {6, 22},                /* 3 */
    {6, 26},                /* 4 */
    {6, 30},                /* 5 */
    {6, 34},                /* 6 */
    {6, 22, 38},            /* 7 */
    {6, 24, 42},            /* 8 */
    {6, 26, 46},            /* 9 */
    {6, 28, 50},            /* 10 */
    {6, 30, 54},            /* 11 */
    {6, 32, 58},            /* 12 */
    {6, 34, 62},            /* 13 */
    {6, 26, 46, 66},        /* 14 */
    {6, 26, 48, 70},        /* 15 */
};
static const int ALIGN_COUNT[NP_QR_MAX_VERSION + 1] = {
    0, 0, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 4, 4
};

static void draw_function_patterns(matrix *m, int version, int mask) {
    memset(m->fn, 0, sizeof m->fn);
    memset(m->mod, 0, sizeof m->mod);
    draw_timing(m);              /* first: finder patterns overwrite its ends */
    draw_finder(m, 3, 3);
    draw_finder(m, m->size - 4, 3);
    draw_finder(m, 3, m->size - 4);
    /* All pairwise alignment centers except the three finder corners. */
    int cnt = ALIGN_COUNT[version];
    for (int i = 0; i < cnt; i++) {
        for (int j = 0; j < cnt; j++) {
            int x = ALIGN_POS[version][i];
            int y = ALIGN_POS[version][j];
            if ((i == 0 && j == 0) ||
                (i == 0 && j == cnt - 1) ||
                (i == cnt - 1 && j == 0)) continue;
            draw_alignment(m, x, y);
        }
    }
    draw_version_bits(m, version);
    draw_format_bits(m, mask);
}

static bool mask_bit(int mask, int x, int y) {
    switch (mask) {
        case 0: return (x + y) % 2 == 0;
        case 1: return y % 2 == 0;
        case 2: return x % 3 == 0;
        case 3: return (x + y) % 3 == 0;
        case 4: return (x / 3 + y / 2) % 2 == 0;
        case 5: return (x * y) % 2 + (x * y) % 3 == 0;
        case 6: return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
        case 7: return ((x + y) % 2 + (x * y) % 3) % 2 == 0;
        default: return false;
    }
}

static void draw_codewords(matrix *m, const uint8_t *codewords, size_t nbits, int mask) {
    int i = 0; /* bit index into codewords */
    bool upward = true;
    for (int right = m->size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < m->size; vert++) {
            int y = upward ? (m->size - 1 - vert) : vert;
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                if (m->fn[y][x]) continue;
                bool dark = false;
                if (i < (int)nbits) dark = ((codewords[i >> 3] >> (7 - (i & 7))) & 1) != 0;
                if (mask_bit(mask, x, y)) dark = !dark;
                m->mod[y][x] = dark ? 1 : 0;
                i++;
            }
        }
        upward = !upward;
    }
}

/* Penalty score for mask selection (simplified but standard rules). */
static int penalty(matrix *m) {
    int result = 0;
    int n = m->size;
    /* Adjacent modules in row/column. */
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            bool dark = m->mod[y][x] != 0;
            int run = 1;
            if (x + 4 < n) {
                run = 1;
                for (int dx = 1; dx < 5; dx++) if ((m->mod[y][x + dx] != 0) != dark) run = 0;
                if (run) result += 3;
            }
            if (y + 4 < n) {
                run = 1;
                for (int dy = 1; dy < 5; dy++) if ((m->mod[y + dy][x] != 0) != dark) run = 0;
                if (run) result += 3;
            }
        }
    }
    /* 2x2 blocks of same color. */
    for (int y = 0; y < n - 1; y++) {
        for (int x = 0; x < n - 1; x++) {
            uint8_t c = m->mod[y][x];
            if (m->mod[y][x + 1] == c && m->mod[y + 1][x] == c && m->mod[y + 1][x + 1] == c)
                result += 3;
        }
    }
    /* Balance of dark modules. */
    int dark = 0;
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) dark += m->mod[y][x];
    int total = n * n;
    result += (abs(dark * 20 - total * 10) / total) * 10;
    return result;
}

bool np_qr_encode(const void *data, size_t len, np_qr_code *out) {
    if (!out || len > 512) return false;
    const uint8_t *bytes = (const uint8_t *)data;

    /* Choose the smallest version that fits: data codewords = total - ecc. */
    int version = 0;
    for (int v = 1; v <= NP_QR_MAX_VERSION; v++) {
        int total_cw = TOTAL_CODEWORDS[v];
        int ecc_total = ECC_PER_BLOCK_M[v] * NUM_BLOCKS_M[v];
        int capacity = total_cw - ecc_total;
        /* byte-mode overhead: 4-bit mode + char-count bits (8 for v<10, 16 for v>=10). */
        int cc_bits = v < 10 ? 8 : 16;
        int needed_bits = 4 + cc_bits + (int)len * 8;
        if (needed_bits <= capacity * 8) { version = v; break; }
    }
    if (version == 0) return false;

    int ecc_total = ECC_PER_BLOCK_M[version] * NUM_BLOCKS_M[version];
    int data_cw = TOTAL_CODEWORDS[version] - ecc_total;

    uint8_t raw[NP_QR_MAX_MODULES * NP_QR_MAX_MODULES];
    memset(raw, 0, sizeof raw);
    bitbuf bb = {raw, sizeof raw, 0};
    bb_append_bits(&bb, 0x4, 4); /* byte mode */
    bb_append_bits(&bb, (unsigned)len, version < 10 ? 8 : 16);
    for (size_t i = 0; i < len; i++) bb_append_bits(&bb, bytes[i], 8);
    /* Terminator (up to 4 zero bits), then pad to byte boundary, then pads. */
    int terminator = data_cw * 8 - (int)bb.len;
    if (terminator > 4) terminator = 4;
    if (terminator > 0) bb_append_bits(&bb, 0, terminator);
    while (bb.len % 8 != 0) bb_append_bits(&bb, 0, 1);
    const uint8_t pad[2] = {0xEC, 0x11};
    size_t byte_len = bb.len / 8;
    for (int i = 0; byte_len < (size_t)data_cw; i++, byte_len++)
        raw[byte_len] = pad[i % 2];

    uint8_t all[1024];
    interleave_codewords(raw, data_cw, NUM_BLOCKS_M[version],
                         ECC_PER_BLOCK_M[version], all);
    size_t all_bits = (size_t)TOTAL_CODEWORDS[version] * 8;

    int best_score = -1;
    matrix m;
    memset(&m, 0, sizeof m);
    m.size = 4 * version + 17;

    for (int mask = 0; mask < 8; mask++) {
        draw_function_patterns(&m, version, mask);
        draw_codewords(&m, all, all_bits, mask);
        int score = penalty(&m);
        if (best_score < 0 || score < best_score) {
            best_score = score;
            memcpy(out->modules, m.mod, sizeof m.mod);
        }
    }
    out->size = m.size;
    return true;
}
