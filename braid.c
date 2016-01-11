// braid.c
// Copyright (C) 2016 Mark Adler
// For conditions of distribution and use, see the accompanying LICENSE file.
//
// Merge a series of .br streams into a single stream.  The names of files with
// the streams are provided on the command line, and the combined stream is
// written to stdout.  The input streams are scanned backwards using the
// distances to the previous headers in the stream, and then read forwards to
// write out.  Any input streams that do not have a complete set of distances
// are skipped, with that is noted as a warning.  If all of the input .br files
// have a total uncompressed size, then the output trailer contains a total
// uncompressed size.  If there is more than one embedded brotli stream, then
// the output trailer contains a check value of the individual check values.

#include <stdio.h>
#include <stdint.h>
#include "br.h"
#include "xxhash.h"
#include "try.h"

#define local static

// Return the parity of the low 8 bits of n in the 8th bit.  If this is
// exclusive-or'ed with n, then the result has even (zero) parity.
local inline unsigned parity(unsigned n) {
    return (0x34cb00 >> ((n ^ (n >> 4)) & 0xf)) & 0x80;
}

// Read bytes from a file backwards.  The byte returned is the one that
// precedes the current file position.  The file position is left pointing at
// the byte returned, so that the next call returns the byte before that.
// Throw an error if at the start of the file or if there is an I/O error.
local inline unsigned rget1(FILE *in) {
    int ch;
    if (ftello(in) == 0 ||
        fseeko(in, -1, SEEK_CUR) ||
        (ch = getc(in)) == EOF ||
        fseeko(in, -1, SEEK_CUR))
        throw(1, "premature arrival at start of file");
    return ch;
}

// Get a bidirectional variable-length number from in, reading backwards.
local inline uintmax_t getrbvar(FILE *in) {
    unsigned ch = rget1(in);
    if ((ch & 0x80) == 0)
        throw(3, "high bit not set (end of bidirectional variable length)");
    uintmax_t val = ch & 0x7f;
    do {
        ch = rget1(in);
        val = (val << 7) | (ch & 0x7f);
    } while ((ch & 0x80) == 0);
    return val;
}

// Get one byte in the forward direction.  Throw an error on failure.
local inline unsigned get1(FILE *in) {
    int ch = getc(in);
    if (ch == EOF)
        throw(1, "premature end of file");
    return ch;
}

// Get a forward variable-length unsigned integer from in.
local inline uintmax_t getvar(FILE *in) {
    uintmax_t val = 0;
    unsigned ch;
    unsigned shift = 0;
    do {
        ch = get1(in);
        val |= (uintmax_t)(ch & 0x7f) << shift;
        shift += 7;
    } while ((ch & 0x80) == 0);
    return val;
}

// Get a bidirectional variable-length number from in.
local inline uintmax_t getbvar(FILE *in) {
    unsigned ch = get1(in);
    if ((ch & 0x80) == 0)
        throw(3, "invalid bidirectional integer");
    uintmax_t val = ch & 0x7f;
    unsigned shift = 0;
    do {
        ch = get1(in);
        shift += 7;
        val |= (uintmax_t)(ch & 0x7f) << shift;
    } while ((ch & 0x80) == 0);
    return val;
}

// Write out a bi-directional variable sized integer, where the first and last
// bytes have a high bit of 1, and the intermediate bytes have a high bit of 0.
local void bvar(uintmax_t num, FILE *out) {
    putc(0x80 | (num & 0x7f), out);
    while ((num >>= 7) > 0x7f)
        putc(num & 0x7f, out);
    putc(0x80 | num, out);
}

// Write out k bytes of an integer in little-endian order.  k must be at least
// one.
local void little(uintmax_t num, size_t k, FILE *out) {
    do {
        putc(num, out);
        num >>= 8;
    } while (--k);
}

// Type for a linked list used as a stack of offsets.
typedef struct pos_s pos_t;
struct pos_s {
    off_t off;
    pos_t *next;
};

// Push an offset on to the stack.
local inline void push(off_t off, pos_t **pos)
{
    pos_t *entry = malloc(sizeof(pos_t));
    if (entry == NULL)
        throw(-1, "out of memory");
    entry->off = off;
    entry->next = *pos;
    *pos = entry;
}

// Pop and return an offset from the stack, or -1 if the stack is empty.
local inline off_t pop(pos_t **pos)
{
    pos_t *entry = *pos;
    if (entry == NULL)
        return -1;
    off_t off = entry->off;
    *pos = entry->next;
    free(entry);
    return off;
}

// Scan a .br stream backwards for the positions of the headers and trailer.
// Return the file offsets of the headers after the first as well as of the
// trailer, all as an allocated linked list starting at *pos.  The offset of
// the first header (4) is the first item in the list, and the offset of the
// trailer is the last item in the list.  Any errors, including missing
// distances, are thrown.
local void scan(FILE *in, pos_t **pos) {
    // verify .br signature
    {
        rewind(in);
        unsigned char buf[4];
        if (fread(buf, 1, 4, in) != 4 || memcmp(buf, BR_SIG, 4))
            throw(2, "signature mismatch -- not a .br file");
    }

    // verify the trailer, push the file offset of the trailer, and get the
    // file offset of the last header.
    off_t at;
    {
        fseeko(in, 0, SEEK_END);
        unsigned trail;
        while ((trail = rget1(in)) == 0)    // get final trailer mask
            ;                               // bypass any zero padding
        if (parity(trail) || (trail & BR_CONTENT_TRAIL) == 0 ||
            (trail & BR_CONTENT_EXTRA_MASK))
            throw(3, "invalid final trailer");
        if ((trail & BR_CONTENT_CHECK) != 7)
            fseeko(in, -(1 << (trail & 3)), SEEK_CUR);  // skip check of checks
        if (trail & BR_CONTENT_LEN)
            getrbvar(in);                   // skip total uncompressed length
        off_t dist = 0;
        if (trail & BR_CONTENT_OFF)
            dist = getrbvar(in);            // get distance to last header
        if (trail != (BR_CONTENT_TRAIL | 7))
            if (rget1(in) != trail)         // get leading trailer mask
                throw(3, "invalid trailer mask");
        at = ftello(in);                    // file offset of start of trailer
        if (at > 4 && (trail & BR_CONTENT_OFF) == 0)
            throw(4, "no final distance to previous header");
        *pos = NULL;                        // empty the stack
        push(at, pos);                      // save trailer offset on stack
        if (dist) {
            at -= dist;                     // compute offset of last header
            push(at, pos);                  // save last header offset
        }
    }

    // go through the string of distances until at the first header
    while (at > 4) {                        // first header is at offset 4
        fseeko(in, at, SEEK_SET);           // go to previous header
        unsigned mask = get1(in);           // get header content mask
        if (parity(mask) || (mask & BR_CONTENT_TRAIL))
            throw(3, "invalid header content mask");
        if ((mask & BR_CONTENT_OFF) == 0)
            throw(4, "missing intermediate distance");
        at -= getvar(in);                   // offset of previous header
        push(at, pos);                      // save header position on stack
    }
    if (at != 4)
        throw(3, "invalid distance");

    // now at offset 4, so there is indeed a complete set of distances
}

// Write one byte to out, updating offset and check.
local inline unsigned put1(unsigned val, FILE *out, off_t *off,
                           XXH32_state_t *check) {
    putc(val, out);
    (*off)++;
    if (check != NULL) {
        unsigned char buf[1];
        buf[0] = val;
        XXH32_update(check, buf, 1);
    }
    return val;
}

// Write the variable-length integer n to out, updating off and check.
local inline void putvar(uintmax_t n, FILE *out, off_t *off,
                      XXH32_state_t *check) {
    while (n > 0x7f) {
        put1(n & 0x7f, out, off, check);
        n >>= 7;
    }
    put1(n | 0x80, out, off, check);
}

// Copy len bytes from in to out, updating off and check.
local void copyn(FILE *in, uintmax_t len, FILE *out, off_t *off,
                 XXH32_state_t *check) {
    *off += len;
    unsigned char buf[16384];
    while (len) {
        size_t n = len > sizeof(buf) ? sizeof(buf) : len;
        fread(buf, 1, n, in);
        fwrite(buf, 1, n, out);
        if (ferror(in) || ferror(out))
            throw(1, "input/output error");
        if (check != NULL)
            XXH32_update(check, buf, n);
        len -= n;
    }
}

// Copy the segment pointed to by the front of the *pos list from in to out.
// Update the check of check values.  Pull the offset of the segment copied
// from the stack.  It is assured that there will be at least two elements on
// the stack when copy() is called: the offset of the next header, and the
// offset of the header after that or the trailer.  *last is the offset of the
// last header (zero for the first segment), and is updated to the offset of
// the header written here.  *off is the current offset of the output file, and
// is updated per the data written.
local void copy(FILE *in, FILE *out, off_t *off, off_t *last, pos_t **pos,
                XXH32_state_t *check) {
    // read header content mask and skip distance back if present
    fseeko(in, pop(pos), SEEK_SET);
    unsigned mask = getc(in);
    if (mask & BR_CONTENT_OFF)
        getvar(in);                             // skip old distance

    // header check state (set head to NULL to stop calculation)
    XXH32_state_t xxh32;
    XXH32_state_t *head = &xxh32;
    XXH32_reset(head, 0);

    // write header content mask, and offset if not first segment
    {
        if (*last) {
            mask |= BR_CONTENT_OFF;
            mask ^= parity(mask);
        }
        off_t here = *off;
        put1(mask, out, off, head);
        if (*last)
            putvar(here - *last, out, off, head);
        *last = here;
    }

    // copy the rest of the header
    if ((mask & BR_CONTENT_CHECK) == 7)
        put1(getc(in), out, off, head);     // copy check ID
    if (mask & BR_CONTENT_EXTRA_MASK) {
        unsigned extra = getc(in);
        if ((extra & BR_EXTRA_CHECK) == 0)  // header check if in original
            head = NULL;
        unsigned strip = extra;             // output extra mask
        if (*last != 4)                     // strip mod time and name
            strip &= ~(BR_EXTRA_MOD | BR_EXTRA_NAME);
        put1(strip, out, off, head);        // copy stripped extra mask
        if (extra & BR_EXTRA_MOD) {
            uintmax_t mod = getvar(in);     // get mod time
            if (strip & BR_EXTRA_MOD)
                putvar(mod, out, off, head);    // write mod time
        }
        if (extra & BR_EXTRA_NAME) {
            uintmax_t len = getvar(in);     // get name length
            if (strip & BR_EXTRA_NAME) {
                putvar(len, out, off, head);    // write name length
                copyn(in, len, out, off, head); // copy name
            }
            else
                fseeko(in, len, SEEK_CUR);  // skip name
        }
        if (extra & BR_EXTRA_EXTRA) {
            uintmax_t len = getvar(in);
            putvar(len, out, off, head);    // copy extra field length
            copyn(in, len, out, off, head); // copy extra field
        }
        if (extra & BR_EXTRA_COMPRESSION_MASK)
            put1(getc(in), out, off, head); // copy method mask
        if (head != NULL) {
            getc(in);  getc(in);            // skip old header check
            unsigned x = XXH32_digest(head) & 0xffff;
            put1(x & 0xff, out, off, NULL); // write new header check
            put1(x >> 8, out, off, NULL);
        }
    }

    // copy the rest of the segment, updating check with the check value
    {
        // length of remainder
        uintmax_t len = (*pos)->off - ftello(in);
        // length of check value
        unsigned n = (mask & BR_CONTENT_CHECK) == 7 ? 32 : 1 << (mask & 3);
        copyn(in, len - n, out, off, NULL); // copy brotli stream
        copyn(in, n, out, off, check);      // copy check value
    }
}

int main(int argc, char **argv) {
    int ret = 0;
    unsigned count = 0;                     // count of brotli streams, up to 2
    intmax_t len = 0;                       // total uncompressed length or -1
    XXH32_state_t check;                    // check of check values
    XXH32_reset(&check, 0);
    fwrite(BR_SIG, 1, 4, stdout);           // write signature
    off_t off = 4;                          // offset for stdout
    off_t last = 0;                         // last header offset (none yet)
    while (--argc) {
        FILE *in = fopen(*++argv, "rb");
        pos_t *pos;
        int any;
        ball_t err;
        try {
            if (in == NULL)
                throw(1, "could not open %s", *argv);
            scan(in, &pos);
            any = pos->next != NULL;
        }
        preserve {                          // preserve pos
            // copy segments
            while (pos->next != NULL) {
                copy(in, stdout, &off, &last, &pos, &check);
                if (count < 2)
                    count++;
            }
        }
        preserve {                          // preserve pos
            // look at trailer to update uncompressed length -- if there is no
            // length, then give up and don't try this again
            if (len != -1) {
                unsigned trail = getc(in);
                if (trail & BR_CONTENT_LEN) {
                    if (trail & BR_CONTENT_OFF)
                        getbvar(in);        // skip offset
                    len += getbvar(in);     // add uncompressed length
                }
                else if (any)
                    len = -1;
            }

            // check for any i/o errors
            if (ferror(in) || ferror(stdout))
                throw(1, "input/output error");
        }
        always {
            while (pos != NULL)             // clean up any leftover mallocs
                pop(&pos);
            fclose(in);
        }
        catch (err) {
            fprintf(stderr, "braid: %s in %s -- skipping\n", err.why, *argv);
            drop(err);
            ret = 1;
        }
    }

    // write trailer
    unsigned trail = BR_CONTENT_TRAIL | (count > 1 ? BR_CHECK_XXH32_4 : 7);
    if (count == 0)
        len = -1;
    if (len != -1)
        trail |= BR_CONTENT_LEN;
    if (last)
        trail |= BR_CONTENT_OFF;
    trail ^= parity(trail);
    putchar(trail);                         // write trailer mask
    if (last)
        bvar(off - last, stdout);           // write offset to last header
    if (len != -1)
        bvar(len, stdout);                  // write total length
    if (count > 1)
        little(XXH32_digest(&check), 4, stdout);    // write check
    if (trail != (BR_CONTENT_TRAIL | 7))
        putchar(trail);                     // write final trailer mask
    if (ferror(stdout) || fclose(stdout)) {
        fprintf(stderr, "braid: output error\n");
        ret = 1;
    }
    return ret;
}
