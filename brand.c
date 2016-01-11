// brand.c
// Copyright (C) 2016 Mark Adler
// For conditions of distribution and use, see the accompanying LICENSE file.
//
// Wrap a raw brotli stream from stdin with the framing format, writing the
// result to stdout.  The stream is decoded in order to generate a check value.
// This code is to illustrate and test the use of the .br framing format.  The
// entire input is loaded into memory, so it is not intended for production
// use.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <openssl/sha.h>
#include "load.h"
#include "yeast.h"
#include "br.h"
#include "xxhash.h"
#include "crc32c.h"

#define local static

// Issue a warning.
local void warn(char *fmt, ...) {
    fputs("wrap warning: ", stderr);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    putc('\n', stderr);
}

// Return the parity of the low 8 bits of n in the 8th bit.  If this is
// exclusive-or'ed with n, then the result has even (zero) parity.
local inline unsigned parity(unsigned n) {
    return (0x34cb00 >> ((n ^ (n >> 4)) & 0xf)) & 0x80;
}

// Write out k bytes of an integer in little-endian order.  k must be at least
// one.
local void little(uintmax_t num, size_t k, FILE *out) {
    do {
        putc(num, out);
        num >>= 8;
    } while (--k);
}

// Write out a bi-directional variable sized integer, where the first and last
// bytes have a high bit of 1, and the intermediate bytes have a high bit of 0.
// Return the number of bytes written.
local size_t bvar(uintmax_t num, FILE *out) {
    size_t n = 2;
    putc(0x80 | (num & 0x7f), out);
    while ((num >>= 7) > 0x7f) {
        putc(num & 0x7f, out);
        n++;
    }
    putc(0x80 | num, out);
    return n;
}

// Write out a variable size integer, where the last byte has a high bit of
// 1.  Return the number of bytes written.
local size_t var(uintmax_t num, FILE *out) {
    size_t n = 1;
    while (num > 0x7f) {
        putc(num & 0x7f, out);
        num >>= 7;
        n++;
    }
    putc(0x80 | num, out);
    return n;
}

// Write the compressed stream br[0..len-1] wrapped with check value check.
// opt provides these options to use for wrapping (any order):
//
//   1 - 1-byte check value
//   2 - 2-byte check value
//   4 - 4-byte check value
//   8 - 8-byte check value
//   x - XXH32 (1, 2, or 4) or XXH64 (8)
//   c - CRC-32C (1, 2, or 4)
//   s - SHA-256 (32)
//   n - include nothing in the trailer (one byte)
//   u - include uncompressed length in trailer
//   r - include reverse offset in trailer
//   b - include both length and offset in trailer
//
local void wrap(void const *brotli, size_t len, void const *un, size_t got,
                char *opt, char *name, FILE *out) {
    // defaults
    enum {
        xxh32, xxh64, crc32, sha256
    } check_type = xxh64;               // use xxh64
    size_t check_len = 8;               // all 8 bytes (required for xxh64)
    int set = 0;                        // true if base check type option seen
    int tail = BR_CONTENT_LEN | BR_CONTENT_OFF; // tail has length and offset
    int mod = 0;                        // no mod time
    int file = 0;                       // no filename

    // interpret options
    while (*opt) {
        // apply option
        switch (*opt) {
            case '1':
            case '2':
            case '4':
            case '8':
                check_len = *opt - '0';
                switch (check_type) {
                    case xxh32:
                        if (check_len == 8)
                            check_type = xxh64;
                        break;
                    case xxh64:
                        if (check_len < 8)
                            check_type = xxh32;
                        break;
                    case crc32:
                        if (check_len == 8) {
                            check_len = 4;
                            warn("%c ignored -- using 4-byte CRC-32C", *opt);
                        }
                        break;
                    case sha256:
                        check_len = 32;
                        warn("%c ignored -- using 32-byte SHA-256", *opt);
                }
                break;
            case 'c':
                if (check_len > 4)
                    check_len = 4;
                if (set && check_type != crc32)
                    warn("%c discarded -- using %d-byte CRC-32C",
                         check_type == sha256 ? 's' : 'x', check_len);
                check_type = crc32;
                set = 1;
                break;
            case 's':
                if (set && check_type != sha256)
                    warn("%c discarded -- using 32-byte SHA-256",
                         check_type == crc32 ? 'c' : 'x');
                check_type = sha256;
                check_len = 32;
                set = 1;
                break;
            case 'x':
                if (check_len > 8)
                    check_len = 8;
                if (set && check_type != xxh32 && check_type != xxh64)
                    warn("%c discarded -- using %d-byte XXH%s",
                         check_type == crc32 ? 'c' : 's', check_len,
                         check_len < 8 ? "32" : "64");
                check_type = check_len < 8 ? xxh32 : xxh64;
                set = 1;
                break;
            case 'n':
                tail = 0;
                break;
            case 'u':
                tail = BR_CONTENT_LEN;
                break;
            case 'r':
                tail = BR_CONTENT_OFF;
                break;
            case 'b':
                tail = BR_CONTENT_LEN | BR_CONTENT_OFF;
                break;
            case 'm':
                mod = 1;
                break;
            case 'f':
                file = 1;
        }

        // process next option
        opt++;
    }

    // write signature
    fwrite("\xce\xb2\xcf\x81", 1, 4, out);

    // write header
    off_t writ = 0;
    unsigned mask = 0;                              // content mask
    switch (check_type) {
        case xxh64:
            mask |= BR_CHECK_XXH64_8;
            break;
        case xxh32:
            mask |= check_len == 4 ? BR_CHECK_XXH32_4 :
                    check_len == 2 ? BR_CHECK_XXH32_2 :
                    BR_CHECK_XXH32_1;
            break;
        case crc32:
            mask |= check_len == 4 ? BR_CHECK_CRC32_4 :
                    check_len == 2 ? BR_CHECK_CRC32_2 :
                    BR_CHECK_CRC32_1;
            break;
        case sha256:
            mask |= BR_CHECK_ID;
    }
    if (mod || name)
        mask |= BR_CONTENT_EXTRA_MASK;
    putc(mask ^ parity(mask), out);                 // write content mask byte
    writ++;
    if ((mask & 7) == BR_CHECK_ID) {
        putc(0, out);                               // write SHA-256 id byte
        writ++;
    }
    if (mask & BR_CONTENT_EXTRA_MASK) {
        unsigned extra = 0;
        if (mod)
            extra |= BR_EXTRA_MOD;
        if (file)
            extra |= BR_EXTRA_NAME;
        putc(extra ^ parity(extra), out);           // write extra mask byte
        writ++;
        if (mod)                                    // write mod time
            // add 35 seconds for TAI-UTC as of this writing -- need a table of
            // leap seconds to do this right, in case there are more added
            writ += var(((uintmax_t)time(NULL) + 35) << 1, out);
        if (file) {                                 // write file name
            size_t len = strlen(name);
            writ += var(len, out);
            fwrite(name, 1, len, out);
            writ += len;
        }
    }

    // write compressed data
    writ += fwrite(brotli, 1, len, out);

    // write check value
    if (check_type == sha256) {
        unsigned char sha[SHA256_DIGEST_LENGTH];
        SHA256(un, got, sha);
        fwrite(sha, 1, SHA256_DIGEST_LENGTH, out);
    }
    else {
        uint64_t check =
            check_type == xxh64 ? XXH64(un, got, 0) :
            check_type == xxh32 ? XXH32(un, got, 0) :
            check_type == crc32 ? crc32c(0, un, got) : 0;
        little(check, check_len, out);              // write check value
    }
    writ += check_len;

    // write trailer
    tail |= 7 | BR_CONTENT_TRAIL;                   // build trailer mask byte
    tail ^= parity(tail);
    putc(tail, out);                                // write trailer mask byte
    if (tail & BR_CONTENT_OFF)
        bvar(writ, stdout);                         // offset back
    if (tail & BR_CONTENT_LEN)
        bvar(got, stdout);                          // uncompressed length
    if (tail & (BR_CONTENT_OFF | BR_CONTENT_LEN))
        putc(tail, out);                            // repeat mask byte
}

// Wrap brotli stream from stdin, writing the result to stdout.  If there is
// an argument, it is options for the wrapper as a string of characters with
// no spaces.  The defaults are an 8-byte XXH64 check and an uncompressed
// length and reverse offset at the end.  The options are:
//
//  x - use XXH32 or XXH64 -- default
//  c - use CRC-32C
//  s - use SHA-256
//  1 - use a 1-byte check value (XXH32 or CRC-32C)
//  2 - use a 2-byte check value (XXH32 or CRC-32C)
//  4 - use a 4-byte check value (XXH32 or CRC-32C)
//  8 - use an 8-byte check value (XXH64 only) -- default
//  n - do not include uncompressed size or reverse offset at end
//  u - include just the uncompressed size (no reverse offset)
//  r - include just the reverse offset (no uncompressed size)
//  b - include both the uncompressed size and reverse offset -- default
//  f - store second argument as file name (no 2nd arg uses "filename")
//  m - save the current time as the mod time
//
// If there is a second argument, and the f option is provided, then the
// second argument is stored as the file name.
int main(int argc, char **argv) {
    // read in compressed data
    void *brotli = NULL;
    size_t len, size;
    int ret = load(stdin, 0, &brotli, &size, &len);
    if (ret) {
        fputs("wrap: could not load stream from stdin -- aborting\n", stderr);
        return 1;
    }

    // decompress
    void *un;
    size_t got, used = len;
    ret = yeast(&un, &got, brotli, &used, 0);
    if (ret || used != len) {
        free(un);
        fputs("wrap: error decompressing stream -- aborting\n", stderr);
        return 1;
    }

    // write out wrapped compressed data using defaults
    wrap(brotli, len, un, got,
         argc > 1 ? argv[1] : "", argc > 2 ? argv[2] : "filename", stdout);

    // clean up
    free(un);
    free(brotli);
    return 0;
}
