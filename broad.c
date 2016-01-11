// broad.c
// Copyright (C) 2016 Mark Adler
// For conditions of distribution and use, see the accompanying LICENSE file.
//
// Decompress and check a wrapped (.br) brotli stream from stdin to stdout.
// This code is to illustrate and test the use of the .br framing format.  The
// entire input is loaded into memory, so it is not intended for production
// use.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <openssl/sha.h>
#include "load.h"
#include "yeast.h"
#include "br.h"
#include "xxhash.h"
#include "crc32c.h"
#include "try.h"

#define local static

// Type for access to a sequence of bytes in memory with a current pointer and
// an accumulated check value.
typedef struct {
    unsigned char *buf;     // allocated buffer
    size_t next;            // next index to fetch from buffer
    size_t len;             // number of bytes in the buffer
    size_t size;            // allocated size of buffer for resizing
    XXH32_state_t check;    // check value state
} seq_t;

// Initialize a sequence type.
local inline void seq_init(seq_t *seq) {
    seq->buf = NULL;
    seq->next = 0;
    seq->len = 0;
    seq->size = 0;
    XXH32_reset(&seq->check, 0);
}

// Skip n bytes in the sequence.  If there are less than n bytes left, throw an
// error and put the pointer at the end.  If check is true, then update
// seq->check with the skipped bytes.
local inline void skip(seq_t *seq, size_t n, int check) {
    size_t pass = seq->len - seq->next;
    if (pass > n)
        pass = n;
    if (check)
        XXH32_update(&seq->check, seq->buf + seq->next, pass);
    seq->next += pass;
    if (pass < n)
        throw(2, "premature eof");
}

// Get one byte from the sequence.  Throw an error if there are no more bytes.
// Update the check value with the byte.
local inline unsigned get1(seq_t *seq) {
    if (seq->next == seq->len)
        throw(2, "premature eof");
    XXH32_update(&seq->check, seq->buf + seq->next, 1);
    return seq->buf[seq->next++];
}

// Get a little-endian fixed-length n-byte unsigned integer from the sequence.
local inline uintmax_t getn(seq_t *seq, unsigned n) {
    uintmax_t val = 0;
    unsigned shift = 0;
    while (n) {
        val |= (uintmax_t)get1(seq) << shift;
        shift += 8;
        n--;
    }
    return val;
}

// Get a forward variable-length unsigned integer from the sequence.
local inline uintmax_t getvar(seq_t *seq) {
    uintmax_t val = 0;
    unsigned ch;
    unsigned shift = 0;
    do {
        ch = get1(seq);
        val |= (uintmax_t)(ch & 0x7f) << shift;
        shift += 7;
    } while ((ch & 0x80) == 0);
    return val;
}

// Get a bidirectional variable-length unsigned integer from the sequence.
local inline uintmax_t getbvar(seq_t *seq) {
    unsigned ch = get1(seq);
    if ((ch & 0x80) == 0)
        throw(3, "invalid bidirectional integer");
    uintmax_t val = ch & 0x7f;
    unsigned shift = 0;
    do {
        ch = get1(seq);
        shift += 7;
        val |= (uintmax_t)(ch & 0x7f) << shift;
    } while ((ch & 0x80) == 0);
    return val;
}

// Check state for all running three types at the same time.
typedef struct {
    XXH32_state_t xxh32;
    XXH64_state_t xxh64;
    uint32_t crc;
} check_t;

// Update or initialize a check state.
local void update_check(check_t *state, void *buf, size_t len) {
    if (buf == NULL) {
        XXH32_reset(&state->xxh32, 0);
        XXH64_reset(&state->xxh64, 0);
        state->crc = 0;
        return;
    }
    XXH32_update(&state->xxh32, buf, len);
    XXH64_update(&state->xxh64, buf, len);
    state->crc = crc32c(state->crc, buf, len);
}

// Get the requested check value from the check state.  The low three bits of
// type are taken to be a content mask check type.
local uintmax_t get_check(check_t *state, unsigned type) {
    uintmax_t mask = ((1 << ((1 << ((type & 3) + 3)) - 1)) << 1) - 1;
    return (type & BR_CONTENT_CHECK) < 3 ? XXH32_digest(&state->xxh32) & mask :
           (type & BR_CONTENT_CHECK) == 3 ? XXH64_digest(&state->xxh64) :
           state->crc & mask;
}

// Return the parity of the low 8 bits of n in the 8th bit.  If this is
// exclusive-or'ed with n, then the result has even (zero) parity.
local inline unsigned parity(unsigned n) {
    return (0x34cb00 >> ((n ^ (n >> 4)) & 0xf)) & 0x80;
}

// Process framed brotli input from in, writing decompressed data to out.
int broad(FILE *in, FILE *out, int verbose, int write) {
    ball_t err;
    seq_t seq;
    seq_init(&seq);
    uintmax_t total = 0;            // total uncompressed length
    check_t double_check;           // check of individual check values
    update_check(&double_check, NULL, 0);
    int ret = load(in, 0, (void **)&seq.buf, &seq.size, &seq.len);
    try {
        if (ret)
            throw(1, "could not load input");

        // check .br signature
        if (getn(&seq, 4) != 0x81cfb2ce)
            throw(3, "invalid format -- bad signature");

        // go through sequence of chunks
        unsigned mask;
        size_t last;                            // offset of last header
        size_t curr = 0;                        // offset of current header
        for (;;) {
            // update offsets of headers
            last = curr;
            curr = seq.next;

            // process chunk header
            XXH32_reset(&seq.check, 0);         // in case of header check
            mask = get1(&seq);                  // content mask
            if (parity(mask))
                throw(3, "invalid format -- bad content mask parity");
            if (mask & BR_CONTENT_TRAIL)        // trailer
                break;
            if (last == 0 && (mask & BR_CONTENT_OFF))
                throw(3, "invalid format -- reverse offset in first header");
            if (verbose)
                fputs("header\n", stderr);
            if (mask & BR_CONTENT_OFF) {        // reverse offset
                if (curr - last != getvar(&seq))
                    throw(3, "invalid format -- incorrect reverse offset");
                if (verbose)
                    fprintf(stderr, "  offset %ju to previous header\n",
                            curr - last);
            }
            if ((mask & BR_CONTENT_CHECK) == BR_CHECK_ID) {
                unsigned id = get1(&seq);       // check id
                if (id != BR_CHECKID_SHA256)    // only SHA256 defined for now
                    throw(3, "invalid format -- unknown check id");
                if (verbose)
                    fprintf(stderr, "  check id %u\n", id);
            }
            if (mask & BR_CONTENT_EXTRA_MASK) {
                unsigned extra = get1(&seq);    // extra mask
                if (parity(extra) || (extra & BR_EXTRA_RESERVED))
                    throw(3, "invalid format -- extra parity");
                if (verbose)
                    fputs( "  extra\n", stderr);
                if (extra & BR_EXTRA_MOD) {     // modified time (discard)
                    time_t mod = getvar(&seq);
                    if (verbose) {
                        mod = mod & 1 ? -(mod >> 1) - 35 : (mod >> 1) - 35;
                        fprintf(stderr, "    modification time %s",
                                ctime(&mod));
                    }
                }
                if (extra & BR_EXTRA_NAME) {    // file name (discard)
                    uintmax_t n = getvar(&seq);
                    skip(&seq, n, 1);
                    if (verbose) {
                        fputs("    name ", stderr);
                        fwrite(seq.buf + seq.next - n, 1, n, stderr);
                        putc('\n', stderr);
                    }
                }
                if (extra & BR_EXTRA_EXTRA) {   // extra field (discard)
                    uintmax_t n = getvar(&seq);
                    if (verbose)
                        fprintf(stderr, "    extra field of %ju bytes\n", n);
                    skip(&seq, n, 1);
                }
                if (extra & BR_EXTRA_COMPRESSION_MASK) {    // method mask
                    unsigned method = get1(&seq);
                    if (parity(method) ||
                        (method & (BR_COMPRESSION_METHOD |  // only 0 defined
                                   BR_COMPRESSION_RESERVED)))
                        throw(3, "invalid format -- method parity");
                    if (verbose)
                        fprintf(stderr, "    method %u, constraints %u\n",
                                method & 7, (method >> 3) & 7);
                }
                if (extra & BR_EXTRA_CHECK) {       // header check
                    unsigned check = XXH32_digest(&seq.check) & 0xffff;
                    if (check != getn(&seq, 2))
                        throw(3, "invalid format -- header check mismatch");
                    if (verbose)
                        fprintf(stderr, "    header check 0x%04x\n", check);
                }
            }

            // decompress, check, and write
            void *un = NULL;
            size_t got, used = seq.len;
            ret = yeast(&un, &got, seq.buf + seq.next, &used, 0);
            seq.next += used;
            total += got;
            ball_t err;
            try {
                if (ret)
                    throw(4, "invalid compressed data");
                if (verbose)
                    fprintf(stderr,
                            "  brotli %ju compressed, %ju uncompressed\n",
                            used, got);

                // compare uncompressed length with stream
                if (mask & BR_CONTENT_LEN) {
                    if (got != getvar(&seq))
                        throw(5, "uncompressed length mismatch");
                }

                // compare check value of uncompressed data with stream
                unsigned n;
                if ((mask & BR_CONTENT_CHECK) == 7) {   // SHA-256
                    n = SHA256_DIGEST_LENGTH;
                    unsigned char sha[n];
                    SHA256(un, got, sha);
                    skip(&seq, n, 0);
                    if (memcmp(sha, seq.buf + seq.next - n, n))
                        throw(5, "uncompressed check mismatch (SHA-256)");
                    if (verbose) {
                        fputs("  SHA-256 0x", stderr);
                        for (unsigned k = 0; k < n; k++)
                            fprintf(stderr, "%02x", sha[k]);
                        putc('\n', stderr);
                    }
                }
                else {
                    uintmax_t check =
                        (mask & BR_CONTENT_CHECK) < 3 ? XXH32(un, got, 0) :
                        (mask & BR_CONTENT_CHECK) == 3 ? XXH64(un, got, 0) :
                        crc32c(0, un, got);
                    n = 1 << (mask & 3);
                    if (n < 4)
                        check &= n == 2 ? 0xffff : 0xff;
                    if (check != getn(&seq, n))
                        throw(5, "uncompressed check mismatch");
                    if (verbose)
                        fprintf(stderr, "  %s %0*jx\n",
                                (mask & BR_CONTENT_CHECK) >= 4 ? "CRC-32C" :
                                (mask & BR_CONTENT_CHECK) == 3 ? "XXH64" :
                                                                 "XXH32",
                                2 << (mask & 3), check);
                }
                update_check(&double_check, seq.buf + seq.next - n, n);

                // write out uncompressed data
                if (write) {
                    fwrite(un, 1, got, out);
                    if (ferror(out))
                        throw(6, "write error");
                }
            }
            always
                free(un);
            catch (err)
                punt(err);
        }

        // fell out of the loop above with a trailer mask -- process trailer
        if (mask & BR_CONTENT_EXTRA_MASK)       // no extra on trailer
            throw(3, "invalid format -- extra on trailer");
        if (verbose)
            fputs("trailer\n", stderr);
        if (mask & BR_CONTENT_OFF) {            // reverse offset
            if (curr - last != getbvar(&seq))
                throw(3, "invalid format -- incorrect final reverse offset");
            if (verbose)
                fprintf(stderr, "  offset %ju to previous header\n",
                        curr - last);
        }
        if (mask & BR_CONTENT_LEN) {            // uncompressed length
            if (total != getbvar(&seq))
                throw(5, "uncompressed total length mismatch");
            if (verbose)
                fprintf(stderr, "  total length %ju\n", total);
        }
        if ((mask & BR_CONTENT_CHECK) != 7) {   // trailer check of checks
            uintmax_t check = getn(&seq, 1 << (mask & 3));
            if (get_check(&double_check, mask) != check)
                throw(5, "uncompressed double-check mismatch");
            if (verbose)
                fprintf(stderr, "  total %s %0*jx\n",
                        (mask & BR_CONTENT_CHECK) >= 4 ? "CRC-32C" :
                        (mask & BR_CONTENT_CHECK) == 3 ? "XXH64" :
                                                         "XXH32",
                        2 << (mask & 3), check);
        }
        if ((mask & BR_CONTENT_CHECK) != 7 ||
            (mask & (BR_CONTENT_LEN | BR_CONTENT_OFF)))
            if (get1(&seq) != mask)             // final trailer content mask
                throw(3, "invalid format -- trailer mask mismatch");
    }
    always {
        free(seq.buf);
        seq_init(&seq);
    }
    catch (err) {
        fprintf(stderr, "broad() error: %s\n", err.why);
        drop(err);
        return err.code;
    }
    return 0;
}

int main(int argc, char **argv) {
    int verbose = 0;
    int write = 1;
    while (--argc) {
        char *opt = *++argv;
        if (*opt != '-') {
            fprintf(stderr, "broad: %s ignored (not an option)\n", opt);
            continue;
        }
        while (*++opt)
            switch (*opt) {
                case 'v':
                    verbose = 1;
                    break;
                case 't':
                    write = 0;
                    break;
                default:
                    fprintf(stderr, "broad: unknown option %c\n", *opt);
            }
    }
    broad(stdin, stdout, verbose, write);
    return 0;
}
