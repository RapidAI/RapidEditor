/* Verify every built-in binary template validates and parses a minimal sample. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "np_bt.h"
#include "np_bt_templates.h"

typedef struct {
    const char *name;
    unsigned char bytes[256];
    size_t len;
} sample;

static void put16le(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put16be(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static void put32le(unsigned char *p, unsigned v) { put16le(p, v & 0xFFFF); put16le(p + 2, v >> 16); }
static void put32be(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }

/* Build a minimal but realistic file for each format. */
static int make_sample(const char *name, sample *s) {
    unsigned char *b = s->bytes;
    memset(s, 0, sizeof *s);
    if (!strcmp(name, "bmp")) {
        memcpy(b, "BM", 2); put32le(b + 2, 58); put32le(b + 10, 54);
        put32le(b + 14, 40); put32le(b + 18, 2); put32le(b + 22, 2);
        put16le(b + 26, 1); put16le(b + 28, 24);
        s->len = 58;
    } else if (!strcmp(name, "png")) {
        unsigned char sig[8] = {0x89,'P','N','G',13,10,26,10}; memcpy(b, sig, 8);
        put32be(b + 8, 13); memcpy(b + 12, "IHDR", 4);
        put32be(b + 16, 2); put32be(b + 20, 2);
        put32be(b + 33, 0); memcpy(b + 37, "IEND", 4); put32be(b + 41, 0);
        s->len = 49;
    } else if (!strcmp(name, "elf64")) {
        unsigned char e[16] = {0x7F,'E','L','F',2,1,1,0}; memcpy(b, e, 16);
        put16le(b + 16, 2); put16le(b + 18, 62); put32le(b + 20, 1);
        put32le(b + 52, 64); put16le(b + 54, 64); put16le(b + 56, 0);
        put16le(b + 58, 64); put16le(b + 60, 0); put16le(b + 62, 0);
        s->len = 64;
    } else if (!strcmp(name, "elf32")) {
        unsigned char e[16] = {0x7F,'E','L','F',1,1,1,0}; memcpy(b, e, 16);
        put16le(b + 16, 2); put16le(b + 18, 3); put32le(b + 20, 1);
        put32le(b + 24, 0x8048000u); put32le(b + 28, 52); put32le(b + 32, 0);
        put16le(b + 40, 52); put16le(b + 42, 32); put16le(b + 44, 0);
        put16le(b + 46, 40); put16le(b + 48, 0); put16le(b + 50, 0);
        s->len = 52;
    } else if (!strcmp(name, "wav")) {
        memcpy(b, "RIFF", 4); put32le(b + 4, 44); memcpy(b + 8, "WAVE", 4);
        memcpy(b + 12, "fmt ", 4); put32le(b + 16, 16);
        put16le(b + 20, 1); put16le(b + 22, 2); put32le(b + 24, 44100);
        put32le(b + 28, 176400); put16le(b + 32, 4); put16le(b + 34, 16);
        memcpy(b + 36, "data", 4); put32le(b + 40, 8);
        memset(b + 44, 0, 8); s->len = 52;
    } else if (!strcmp(name, "zip")) {
        put32le(b, 0x04034B50); put16le(b + 4, 20); put16le(b + 6, 0);
        put16le(b + 8, 0); put32le(b + 14, 0); put32le(b + 18, 4);
        put32le(b + 22, 4); put16le(b + 26, 4); put16le(b + 28, 0);
        memcpy(b + 30, "test", 4); memcpy(b + 34, "DATA", 4);
        s->len = 38;
    } else if (!strcmp(name, "jpeg")) {
        b[0] = 0xFF; b[1] = 0xD8; b[2] = 0xFF; b[3] = 0xE0;
        put16be(b + 4, 16); memcpy(b + 6, "JFIF\0", 5);
        b[20] = 0xFF; b[21] = 0xD9; s->len = 22;
    } else if (!strcmp(name, "gif")) {
        memcpy(b, "GIF89a", 6); put16le(b + 6, 2); put16le(b + 8, 2);
        b[10] = 0; b[11] = 0; b[12] = 0;
        b[13] = 0x2C; put16le(b + 14, 0); put16le(b + 16, 0);
        put16le(b + 18, 2); put16le(b + 20, 2); b[22] = 0;
        b[23] = 2; b[24] = 1; b[25] = 0; b[26] = 0; b[27] = 0;
        b[28] = 0x3B; s->len = 29;
    } else if (!strcmp(name, "ico")) {
        put16le(b, 0); put16le(b + 2, 1); put16le(b + 4, 1);
        b[6] = 16; b[7] = 16; b[8] = 0; b[9] = 0;
        put16le(b + 10, 1); put16le(b + 12, 32);
        put32le(b + 14, 8); put32le(b + 18, 22);
        s->len = 30;
    } else if (!strcmp(name, "webp")) {
        memcpy(b, "RIFF", 4); put32le(b + 4, 24); memcpy(b + 8, "WEBP", 4);
        memcpy(b + 12, "VP8 ", 4); put32le(b + 16, 4);
        memcpy(b + 20, "ABCD", 4); s->len = 24;
    } else if (!strcmp(name, "mp3")) {
        memcpy(b, "ID3", 3); b[3] = 4; b[4] = 0; b[5] = 0;
        b[6] = 0; b[7] = 0; b[8] = 0; b[9] = 10; /* size=10 */
        memset(b + 10, 0, 10); s->len = 20;
    } else if (!strcmp(name, "flac")) {
        memcpy(b, "fLaC", 4); b[4] = 0x80; b[5] = 0; b[6] = 0; b[7] = 34;
        memset(b + 8, 0, 34); s->len = 42;
    } else if (!strcmp(name, "ogg")) {
        memcpy(b, "OggS", 4); b[4] = 0; b[5] = 0;
        memset(b + 6, 0, 8); put32le(b + 14, 1); put32le(b + 18, 0);
        put32le(b + 22, 0); b[26] = 1; b[27] = 4; b[28] = 1; b[29] = 1;
        memcpy(b + 30, "ABCD", 4); s->len = 34;
    } else if (!strcmp(name, "midi")) {
        memcpy(b, "MThd", 4); put32be(b + 4, 6);
        put16be(b + 8, 0); put16be(b + 10, 1); put16be(b + 12, 96);
        memcpy(b + 14, "MTrk", 4); put32be(b + 18, 0); s->len = 22;
    } else if (!strcmp(name, "pe32")) {
        b[0] = 'M'; b[1] = 'Z'; put32le(b + 60, 64);
        memcpy(b + 64, "PE\0\0", 4); put16le(b + 68, 0x14c);
        put16le(b + 70, 1); put16le(b + 80, 0); put16le(b + 82, 0);
        /* COFF header at 64, size 24 -> section header starts at 88 */
        b[88] = '.', b[89] = 't', b[90] = 'e', b[91] = 'x', b[92] = 't';
        put32le(b + 96, 16); put32le(b + 100, 0x1000);
        put32le(b + 104, 16); put32le(b + 108, 512);
        s->len = 128;
    } else if (!strcmp(name, "gzip")) {
        b[0] = 0x1F; b[1] = 0x8B; b[2] = 8; b[3] = 0;
        put32le(b + 4, 0); b[8] = 0; b[9] = 3;
        memset(b + 10, 0, 12); s->len = 22;
    } else if (!strcmp(name, "wasm")) {
        b[0] = 0; b[1] = 'a'; b[2] = 's'; b[3] = 'm';
        put32le(b + 4, 1);
        b[8] = 1; b[9] = 4; memcpy(b + 10, "type", 4); s->len = 14;
    } else if (!strcmp(name, "sqlite")) {
        memcpy(b, "SQLite format 3\0", 16);
        put16be(b + 16, 4096); b[18] = 1; b[19] = 1;
        s->len = 100;
    } else if (!strcmp(name, "pcap")) {
        put32le(b, 0xA1B2C3D4); put16le(b + 4, 2); put16le(b + 6, 4);
        put32le(b + 16, 65535); put32le(b + 20, 1);
        put32le(b + 24, 0); put32le(b + 28, 0);
        put32le(b + 32, 4); put32le(b + 36, 4);
        memcpy(b + 40, "DATA", 4); s->len = 44;
    } else if (!strcmp(name, "tiff")) {
        memcpy(b, "II", 2); put16le(b + 2, 42); put32le(b + 4, 8);
        put16le(b + 8, 1);
        put16le(b + 10, 256); put16le(b + 12, 3); put32le(b + 14, 1);
        put32le(b + 18, 4); put32le(b + 22, 0);
        s->len = 26;
    } else {
        return 0;
    }
    return 1;
}

int main(void) {
    int fail = 0;
    for (int i = 0; np_bt_builtins[i].name; i++) {
        const np_bt_builtin *t = &np_bt_builtins[i];
        /* 1) validate against zeros */
        char err[256] = "";
        if (!np_bt_validate(t->source, err, sizeof err)) {
            printf("FAIL %-8s validate: %s\n", t->name, err); fail++; continue;
        }
        /* 2) parse a real-ish sample */
        sample s;
        if (!make_sample(t->name, &s)) {
            printf("SKIP %-8s (no sample builder)\n", t->name); continue;
        }
        np_bt_result r;
        if (!np_bt_execute(t->source, s.bytes, s.len, &r)) {
            printf("FAIL %-8s execute: %s\n", t->name, r.error); fail++;
        } else {
            printf("PASS %-8s fields=%d\n", t->name, r.count);
            if (r.count == 0) { printf("  WARN: zero fields\n"); }
        }
        np_bt_result_free(&r);
    }
    printf("\n%s (%d template(s))\n", fail ? "FAILED" : "ALL PASS", fail);
    return fail ? 1 : 0;
}
