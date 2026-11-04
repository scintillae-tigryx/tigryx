/*
 * Copyright (c) 2026 Scintillae Ltd. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/*
 * fbpng.h - shared stored-deflate PNG writer for backend frame dumps.
 *
 * Split out of fbdump.h so non-GL backends (Metal) can dump a presented
 * frame without pulling in the glReadPixels path. Same rules apply: no
 * zlib/miniz, RGB24 top-row-first input, files are debug artifacts.
 */
#ifndef RELIC_FBPNG_H
#define RELIC_FBPNG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t relic_fbd_crc(const uint8_t *p, size_t n, uint32_t crc) {
    static uint32_t tab[256];
    static int init = 0;
    if (!init) {
        for (int i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
            tab[i] = c;
        }
        init = 1;
    }
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) crc = tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static void relic_fbd_be32(uint8_t *d, uint32_t v) {
    d[0] = (uint8_t)(v >> 24); d[1] = (uint8_t)(v >> 16);
    d[2] = (uint8_t)(v >> 8);  d[3] = (uint8_t)v;
}

static void relic_fbd_chunk(FILE *f, const char *tag,
                            const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    relic_fbd_be32(hdr, len);
    memcpy(hdr + 4, tag, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t crc = relic_fbd_crc((const uint8_t*)tag, 4, 0);
    if (len) crc = relic_fbd_crc(data, len, crc);
    uint8_t c[4];
    relic_fbd_be32(c, crc);
    fwrite(c, 1, 4, f);
}

/* rgb: w*h*3 bytes, top row first. */
static int relic_fbdump_write_png(const char *path, const uint8_t *rgb,
                                  int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    static const uint8_t sig[8] = { 0x89,'P','N','G','\r','\n',0x1A,'\n' };
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    relic_fbd_be32(ihdr,     (uint32_t)w);
    relic_fbd_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;    /* bit depth    */
    ihdr[9] = 2;    /* colour: RGB  */
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    relic_fbd_chunk(f, "IHDR", ihdr, 13);

    /* Raw zlib stream: each scanline is a 0 filter byte + RGB triples. */
    size_t stride = (size_t)w * 3 + 1;
    size_t raw_len = stride * (size_t)h;
    uint8_t *raw = (uint8_t*)malloc(raw_len);
    if (!raw) { fclose(f); return 0; }
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + stride * (size_t)y;
        row[0] = 0;
        memcpy(row + 1, rgb + (size_t)y * (size_t)w * 3, (size_t)w * 3);
    }

    /* zlib wrapper + stored deflate blocks + Adler-32. */
    size_t nblk = (raw_len + 65534) / 65535;
    size_t idat_len = 2 + nblk * 5 + raw_len + 4;
    uint8_t *idat = (uint8_t*)malloc(idat_len);
    if (!idat) { free(raw); fclose(f); return 0; }
    size_t o = 0;
    idat[o++] = 0x78; idat[o++] = 0x01;         /* CM/CINFO, FLG       */
    size_t left = raw_len, off = 0;
    do {
        size_t n = left > 65535 ? 65535 : left;
        idat[o++] = (left - n == 0) ? 1 : 0;    /* BFINAL, BTYPE=00    */
        idat[o++] = (uint8_t)(n & 0xFF);
        idat[o++] = (uint8_t)(n >> 8);
        idat[o++] = (uint8_t)(~n & 0xFF);
        idat[o++] = (uint8_t)((~n >> 8) & 0xFF);
        memcpy(idat + o, raw + off, n);
        o += n; off += n; left -= n;
    } while (left);
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    relic_fbd_be32(idat + o, (b << 16) | a);
    o += 4;

    relic_fbd_chunk(f, "IDAT", idat, (uint32_t)o);
    relic_fbd_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(idat);
    free(raw);
    return 1;
}

#endif /* RELIC_FBPNG_H */
