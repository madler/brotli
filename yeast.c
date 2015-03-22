/*
 * yeast.c
 * Copyright (C) 2015 Mark Adler
 * For conditions of distribution and use, see the accompanying LICENSE file.
 *
 * yeast.c is a simple decompressor of the brotli format, written to both test
 * the completeness and correctness of the brotli specification, and once
 * verified, to provide an unambiguous specification of the format by virtue of
 * being a working decoder.  It is a higher priority for this code be simple
 * and readable than to be fast.
 *
 * This code is intended to be compliant with the C99 standard and portable.
 *
 */

#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <inttypes.h>
#include <assert.h>
#include "try.h"
#include "yeast.h"

/* Verify that size_t is at least 32 bits. */
#if SIZE_MAX < 4294967295
#  error size_t is less than 32 bits
#endif

/* local for functions not linked outside of this module. */
#define local static

/* trace() macro for debugging. */
#ifdef DEBUG
#  include <stdio.h>
#  define trace(...) \
    do { \
        fputs("yeast: ", stderr); \
        fprintf(stderr, __VA_ARGS__); \
        putc('\n', stderr); \
    } while (0)
#else
#  define trace(...)
#endif

/*
 * Assured memory allocation.
 */
local void *alloc(void *mem, size_t size)
{
    mem = realloc(mem, size);
    if (size && mem == NULL)
        throw(1, "out of memory");
    return mem;
}

/* Proper use of plurals for numbers. */
#define PLURAL(n) (n), (n) == 1 ? "" : "s"

/* The maximum number of bits in a prefix code. */
#define MAXBITS 15

/* The maximum number of symbols in an alphabet. */
#define MAXSYMS 704

/*
 * Prefix code decoding table type.  count[0..MAXBITS] are the number of
 * symbols of each length, from which a canonical code is generated.
 * symbol[0..n-1] are the symbol values corresponding in order to the codes
 * from short to long.  n is the sum of the counts in count[].  The decoding
 * process can be seen in the function decode() below. count[] must represent a
 * complete code, and so must satisfy:
 *
 *     sum(count[i] * (1 << (MAXBITS - i)), i=0..MAXBITS) == 1 << MAXBITS
 *
 * If count[0] is 1, then the code has zero bits with a single symbol.  In this
 * case symbol[0] is returned without consuming any bits of input.
 */
typedef struct {
    unsigned short count[MAXBITS+1];    /* number of symbols of each length */
    unsigned short symbol[MAXSYMS];     /* canonically ordered symbols */
} prefix_t;

/*
 * Brotli decoding state.  About 26K bytes, plus allocated prefix codes.
 */
typedef struct {
    /* input state */
    unsigned char const *next;      /* next bytes to get from input buffer */
    size_t len;                     /* number of bytes at next */
    unsigned char bits;             /* bit buffer (holds 0..7 bits) */
    unsigned char left;             /* number of bits left in bit buffer */

    /* sliding window size */
    unsigned short wbits;           /* log2(16 + sliding window size) */
    uint32_t wsize;                 /* sliding window size in bytes */

    /* output state */
    unsigned char *dest;            /* allocated output space */
    size_t got;                     /* bytes written to output so far */

    /* codes types state */
    unsigned short lit_num;         /* number of literal types */
    unsigned short lit_prev;        /* literal type before the last one */
    unsigned short lit_last;        /* literal type before this one */
    unsigned short lit_type;        /* literal type currently in use */
    size_t lit_left;                /* number of literals left of this type */
    unsigned short iac_num;         /* number of insert types */
    unsigned short iac_prev;        /* insert type before the last one */
    unsigned short iac_last;        /* insert type before this one */
    unsigned short iac_type;        /* insert type currently in use */
    size_t iac_left;                /* number of inserts left of this type */
    unsigned short dist_num;        /* number of distance types */
    unsigned short dist_prev;       /* distance type before the last one */
    unsigned short dist_last;       /* distance type before this one */
    unsigned short dist_type;       /* distance type currently in use */
    size_t dist_left;               /* number of distances left of this type */

    /* distance code decoding */
    uint32_t ring[4];               /* ring buffer of previous distances */
    unsigned short ring_ptr;        /* index of last distance in ring buffer */
    unsigned char postfix;          /* log2 of # of interleavings (0..3) */
    unsigned char direct;           /* number of direct distance codes */

    /* codes */
    unsigned short lit_codes;       /* number of literal prefix codes */
    unsigned short dist_codes;      /* number of distance prefix codes */
    prefix_t *lit_code;             /* lit_codes literal codes (allocated) */
    prefix_t *iac_code;             /* iac_num insert codes (allocated) */
    prefix_t *dist_code;            /* dist_codes distance codes (allocated) */

    /* context */
    unsigned char mode[256];        /* modes for lit_num literal types */
    unsigned char lit_map[64*256];  /* literal context map */
    unsigned char dist_map[4*256];  /* distance context map */

    /* codes for prefix code changes */
    prefix_t lit_types;             /* literal block types */
    prefix_t lit_count;             /* literal block lengths */
    prefix_t iac_types;             /* insert and copy block types */
    prefix_t iac_count;             /* insert and copy block lengths */
    prefix_t dist_types;            /* distance block types */
    prefix_t dist_count;            /* distance block lengths */
} state_t;

/*
 * Return need bits from the input stream.  need must be in 0..26.  This will
 * leave 0..7 bits in s->bits.
 *
 * Format notes:
 *
 * - Bits are stored in bytes from the least significant bit to the most
 *   significant bit.  Therefore bits are dropped from the bottom of the bit
 *   buffer, using shift right, and new bytes are appended to the top of the
 *   bit buffer, using shift left.
 */
local uint32_t bits(state_t *s, unsigned need)
{
    uint32_t reg;       /* register in which to accumulate need bits */

    assert(need < 26);
    reg = s->bits;
    while (s->left < need) {
        if (s->len == 0)
            throw(2, "premature end of input");
        reg |= (uint32_t)(*(s->next)++) << s->left;
        s->len--;
        s->left += 8;
    }
    s->bits = reg >> need;
    s->left -= need;
    return reg & (((uint32_t)1 << need) - 1);
}

/*
 * Decode a code from the stream s using prefix table p.  Return the symbol.
 *
 * Format notes:
 *
 * - The codes as stored in the compressed data are bit-reversed relative to a
 *   simple integer ordering of codes of the same lengths.  The bits are pulled
 *   from the compressed data one at a time and used to build the code value
 *   reversed from what is in the stream in order to permit simple integer
 *   comparisons for decoding.
 *
 * - The first code for the shortest non-zero length is all zeros.  Subsequent
 *   codes of the same length are integer increments of the previous code.
 *   When moving up a length, a zero bit is appended to the code.  The last
 *   code of the longest length will be all ones.
 *
 * - A code with a single symbol is permitted, where the number of bits for
 *   that symbol is zero.
 *
 * - All codes in the brotli format are complete, so the only error possible
 *   when decoding a prefix code is running out of input bits.  (bits() will
 *   throw an error in that case.)
 */
local unsigned decode(state_t *s, prefix_t const *p)
{
    unsigned len = 0;       /* current number of bits in code */
    unsigned first = 0;     /* first code of length len */
    unsigned index = 0;     /* index of length len codes in symbol table */
    unsigned code = 0;      /* the len bits being decoded */
    unsigned count;         /* number of codes of length len */

    while (code >= first + (count = p->count[len]) && ++len <= MAXBITS) {
        index += count;                     /* update symbol index for len */
        first = (first + count) << 1;       /* update first code for len */
        code = (code << 1) | bits(s, 1);    /* get next bit */
    }
    assert(len <= MAXBITS);
    return p->symbol[index + code - first];
}

/*
 * Given the list of code lengths length[0..n-1] representing a prefix code for
 * the n symbols 0..n-1, construct the tables required to decode those codes.
 * Those tables are the number of codes of each length, and the symbols sorted
 * by length, and sorted by symbol value within each length.  This assumes that
 * the provided lengths consititute a complete prefix code.  (This must be
 * checked before construct() is called.)
 *
 * length[k] == 0 means that symbol k is not coded.  Otherwise length[k] is the
 * number of bits used for symbol k.
 *
 * Format notes:
 *
 * - The brotli format only permits complete codes.
 *
 * - The brotli format permits codes with a single symbol whose code is zero
 *   bits.  construct() is not called in that case, nor is it called for the
 *   other simple prefix codes since the symbols are provided differently in
 *   that descriptor.  For those, simple() is called instead.  construct() is
 *   only called with complete codes that have at least two symbols.
 *
 * - The brotli format limits the lengths of codes to 15 bits.
 */
local void construct(prefix_t *p, unsigned char const *length, unsigned n)
{
    unsigned symbol;                /* current symbol */
    unsigned len;                   /* current length */
    unsigned slen;                  /* number of bits for this symbol */
    unsigned short offs[MAXBITS+1]; /* symbol offsets for each length */

    assert(n < MAXSYMS);

    /* count number of codes of each non-zero length */
    for (len = 0; len <= MAXBITS; len++)
        p->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) {
        slen = length[symbol];
        if (slen) {
            assert(slen <= MAXBITS);
            (p->count[slen])++;
        }
    }

    /* generate offsets into the symbol table for each length */
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++)
        offs[len + 1] = offs[len] + p->count[len];

    /* put symbols into the table sorted by length, and by symbol order within
       each length */
    for (symbol = 0; symbol < n; symbol++) {
        slen = length[symbol];
        if (slen)
            p->symbol[offs[slen]++] = symbol;
    }
}

/*
 * Swap list[i] and list[j] if they are not in order.
 */
#define ORDER(list, i, j) \
    do { \
        if (list[i] > list[j]) { \
            unsigned short tmp = list[i]; \
            list[i] = list[j]; \
            list[j] = tmp; \
        } \
    } while (0)

/*
 * Construct the tables required to decode the provided simple prefix code.
 * type is 1 for one symbol of zero length; 2 for two symbols each of length 1;
 * 3 for three symbols of code lengths of 1, 2, 2; 4 for four symbols of code
 * lengths 2, 2, 2, 2; and 5 for four symbols of code lengths 1, 2, 3, 3.
 *
 * Format note:
 * - The symbols provided in the stream are in order with respect to the bit
 *   lengths corresponding to the types.  However they may not be in order
 *   within each bit length.  In fact, there are cases in example compressed
 *   data where this is seen.  Here symbols of the same bit length are sorted
 *   in order to generate a canonical code.
 */
local void simple(prefix_t *p, unsigned short const *syms, unsigned type)
{
    unsigned n;

    assert(type >= 1 && type <= 5);

    /* initialize the decoding table */
    for (n = 0; n <= MAXBITS; n++)
        p->count[n] = 0;
    for (n = 0; n < (type > 4 ? 4 : type); n++)
        p->symbol[n] = syms[n];

    /* for each simple code type, set the counts for the lengths used, and sort
       symbols within the same code length */
    switch (type) {
        case 1:
            p->count[0] = 1;
            break;
        case 2:
            p->count[1] = 2;
            ORDER(p->symbol, 0, 1);
            break;
        case 3:
            p->count[1] = 1;
            p->count[2] = 2;
            ORDER(p->symbol, 1, 2);
            break;
        case 4:
            p->count[2] = 4;
            ORDER(p->symbol, 0, 1);
            ORDER(p->symbol, 2, 3);
            ORDER(p->symbol, 0, 2);
            ORDER(p->symbol, 1, 3);
            ORDER(p->symbol, 1, 2);
            break;
        case 5:
            p->count[1] = 1;
            p->count[2] = 1;
            p->count[3] = 2;
            ORDER(p->symbol, 2, 3);
    }
}

/*
 * Read in a prefix code description and save the tables in p.  num is the
 * maximum number of symbols in the alphabet.
 */
local void prefix(state_t *s, prefix_t *p, unsigned num)
{
    unsigned hskip;         /* number of code length code lengths to skip */
    unsigned nsym;          /* number of symbols (some may not be coded) */

    assert(num > 1 && num <= MAXSYMS);

    /* number of leading code length code lengths to skip, or 1 for simple */
    hskip = bits(s, 2);

    /* simple prefix code */
    if (hskip == 1) {
        unsigned abits;             /* alphabet bits */
        unsigned sym;               /* symbol */
        unsigned short syms[4];     /* symbols for this code */
        unsigned n;

        trace("  simple prefix code");

        /* set abits to the number of bits required to represent num - 1 */
        n = 2;
        abits = 1;
        while (n < num) {
            n <<= 1;
            abits++;
        }

        /* read 1..4 symbols */
        nsym = bits(s, 2) + 1;
        for (n = 0; n < nsym; n++) {
            sym = bits(s, abits);
            if (sym >= num)
                throw(3, "modulo really needed?");
            syms[n] = sym;
        }

        /* make nsym 5 for the second 4-symbol simple code */
        if (nsym == 4)
            nsym += bits(s, 1);

        /* generate the simple code */
        simple(p, syms, nsym);
    }

    /* complex prefix code */
    else {
        int32_t left;           /* number of code values left */
        unsigned len;           /* number of bits in code */
        unsigned last;          /* last non-zero length */
        unsigned rep;           /* number of times to repeat last len */
        unsigned zeros;         /* number of times to repeat zero */
        unsigned n;

        /* order of code length code lengths */
        unsigned short const order[] = {
            1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15
        };

        /* initially the code for code length code lengths, then reused for
           the code lengths code */
        prefix_t code = {{0, 0, 3, 1, 2}, {0, 3, 4, 2, 1, 5}};

        /* lengths read for the code lengths code, then reused for the code */
        unsigned char lens[num < 18 ? 18 : num];

        trace("  complex prefix code");

        /* read the code length code lengths using the fixed code length code
           lengths code above, and make the code length code for reading the
           code lengths (seriously) */
        left = 1 << 5;
        nsym = 0;
        while (nsym < hskip)
            lens[order[nsym++]] = 0;
        while (nsym < 18) {
            len = decode(s, &code);
            trace("  (%u,%u)", order[nsym], len);
            lens[order[nsym++]] = len;
            if (len) {
                left -= (1 << 5) >> len;
                if (left <= 0)
                    break;
            }
        }
        if (left < 0)
            throw(3, "oversubscribed code length code");
        if (left)
            throw(3, "incomplete code length code");
        while (nsym < 18)
            lens[order[nsym++]] = 0;
        construct(&code, lens, 18);

        /* read the code lengths */
        left = (int32_t)1 << MAXBITS;
        last = 8;
        rep = 0;
        zeros = 0;
        nsym = 0;
        do {
            len = decode(s, &code);
            if (len < 16) {
                /* not coded (0), or a code length in 1..15 -- only update last
                   if the length is not zero */
                if (nsym == num)
                    throw(3, "too many symbols");
                lens[nsym++] = len;
                if (len) {
                    left -= ((int32_t)1 << MAXBITS) >> len;
                    last = len;
                }
                rep = 0;
                zeros = 0;
            }
            else if (len == 16) {
                /* repeat the last non-zero length (or 8 if no such length) a
                   number of times determined by the next two bits and the
                   previous repeat-last, if the previous one immediately
                   preceded this one */
                n = rep;
                rep = (rep ? (rep - 2) << 2 : 0) + 3 + bits(s, 2);
                n = rep - n;
                if (nsym + n > num)
                    throw(3, "too many symbols");
                left -= n * (((int32_t)1 << MAXBITS) >> last);
                if (left < 0)
                    break;
                do {
                    lens[nsym++] = last;
                } while (--n);
                zeros = 0;
            }
            else {  /* len == 17 */
                /* insert a run of zeros whose length is determined by the next
                   three bits and the length of the previous run of zeroes, if
                   the previous one immediately preceded this one */
                n = zeros;
                zeros = (zeros ? (zeros - 2) << 3 : 0) + 3 + bits(s, 3);
                n = zeros - n;
                if (nsym + n > num)
                    throw(3, "too many symbols");
                do {
                    lens[nsym++] = 0;
                } while (--n);
                rep = 0;
            }
        } while (left > 0);
        if (left < 0)
            throw(3, "oversubscribed code");

        /* make the code */
        construct(p, lens, nsym);
    }

#ifdef DEBUG
    /* show the prefix code */
    {
        unsigned n, k, i;

        i = 0;
        for (n = 0; n <= MAXBITS; n++)
            for (k = 0; k < p->count[n]; k++, i++)
                if (num == 256 && p->symbol[i] >= ' ' && p->symbol[i] <= '~')
                    trace("  %u: '%s%c'",
                          n, p->symbol[i] == '\'' ||
                             p->symbol[i] == '\\' ? "\\" : "",
                          p->symbol[i]);
                else
                    trace("  %u: %u", n, p->symbol[i]);
    }
#endif
}

/* The number of block length codes. */
#define BLOCK_LENGTH_CODES 26

/*
 * Get a block length.
 */
local size_t block_length(state_t *s, prefix_t *p)
{
    unsigned sym;               /* block length symbol */

    /* base value and number of extra bits to add to base value */
    unsigned short const base[] = {
        1, 5, 9, 13, 17, 25, 33, 41, 49, 65, 81, 97, 113, 145, 177, 209, 241,
        305, 369, 497, 753, 1265, 2289, 4337, 8433, 16625
    };
    unsigned char const extra[] = {
        2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 7, 8, 9, 10, 11,
        12, 13, 24
    };

    sym = decode(s, p);
    assert(sym < BLOCK_LENGTH_CODES);
    return (size_t)base[sym] + bits(s, extra[sym]);
}

/*
 * Decode the number of block types.
 *
 * Format note:
 * - Version 02 of the brotli specification is rather misleading on how this is
 *   coded.  The "variable length code" is actually a leading 0 or 1, followed
 *   by a three-bit integer (not a reversed code) which is the number of extra
 *   bits as well as a value from which the base can be readily computed.  That
 *   is followed by the extra bits, not reversed.
 *
 *   The specification shows "1011xxx" for the range 9-16.  So in that format,
 *   the value 13 is 1011100.  If this were actually a variable-length code, it
 *   would be stored in the stream in reverse order, 0011101.  It is not.  If
 *   the first four bits were a prefix code, then they would be stored in
 *   reverse order, with the remaining three bits in normal order, like extra
 *   bits, i.e. 1001101.  It is not. Instead the first bit comes in at the
 *   bottom, followed by the next three bits *not* reversed, followed by the
 *   extra bits not reversed. I.e. 1000111.
 */
local unsigned block_types(state_t *s)
{
    unsigned code;              /* block type code */

    if (bits(s, 1) == 0)
        return 1;
    code = bits(s, 3);
    return 1 + (1 << code) + bits(s, code);
}

/*
 * Read a context map into map[0..len-1], with entries in the range 0..trees-1.
 */
local void context_map(state_t *s, unsigned char *map, size_t len,
                       unsigned trees)
{
    unsigned rlemax;        /* maximum run-length directive */
    unsigned sym;           /* decoded symbol */
    size_t zeros;           /* number of zeros to write */
    unsigned n;             /* map index */
    prefix_t code;          /* map code */

    /* get the code to read the map */
    rlemax = bits(s, 1) ? 1 + bits(s, 4) : 0;
    if ((size_t)1 << rlemax > len)
        throw(3, "rlemax of %u unnecessarily large for map length",
              rlemax);
    trace("%srun length code, rlemax = %u (max run %zu)",
          rlemax ? "" : "no ", rlemax, ((size_t)1 << (rlemax + 1)) - 1);
    trace("context map code (%u+%u):", rlemax, trees);
    prefix(s, &code, rlemax + trees);

    /* read the map, expanding runs of zeros */
    trace("context map:");
    n = 0;
    do {
        sym = decode(s, &code);
        if (sym == 0) {
            map[n++] = 0;
            trace("  value 0 (have %u)", n);
        }
        else if (sym <= rlemax) {
            zeros = ((size_t)1 << sym) + bits(s, sym);
            if (n + zeros > len)
                throw(3, "run length too long");
            trace("  %zu 0's (have %zu)", zeros, n + zeros);
            do {
                map[n++] = 0;
            } while (--zeros);
        }
        else {
            map[n++] = sym - rlemax;
            trace("  value %u (have %u)", sym - rlemax, n);
        }
    } while (n < len);

    /* do an inverse move-to-front transform if requested */
    if (bits(s, 1)) {
        unsigned char table[trees];

        trace("inverse move-to-front");
        for (n = 0; n < trees; n++)
            table[n] = n;
        for (n = 0; n < len; n++) {
            sym = map[n];
            assert(sym < trees);
            map[n] = table[sym];
            if (sym) {
                do {
                    table[sym] = table[sym - 1];
                } while (--sym);
                table[0] = map[n];
            }
        }
    }
}

/*
 * Get an insert length given the insert and copy symbol (0..703).
 */
local size_t insert_length(state_t *s, unsigned sym)
{
    /* map from insert and copy to insert symbol base */
    unsigned char const map[] = {0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16};

    /* base value and number of extra bits to add to base value */
    unsigned short const base[] = {
        0, 1, 2, 3, 4, 5, 6, 8, 10, 14, 18, 26, 34, 50, 66, 98, 130, 194, 322,
        578, 1090, 2114, 6210, 22594
    };
    unsigned char const extra[] = {
        0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 12, 14,
        24
    };

    assert(sym < 704);
    sym = map[sym >> 6] + ((sym >> 3) & 7);
    return (size_t)base[sym] + bits(s, extra[sym]);
}

/*
 * Get a copy length given the insert and copy symbol (0..703).
 */
local size_t copy_length(state_t *s, unsigned sym)
{
    /* map from insert and copy to copy symbol base */
    unsigned char const map[] = {0, 8, 0, 8, 0, 8, 16, 0, 16, 8, 16};

    /* base value and number of extra bits to add to base value */
    unsigned short const base[] = {
        2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 18, 22, 30, 38, 54, 70, 102, 134,
        198, 326, 582, 1094, 2118
    };
    unsigned char const extra[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10,
        24
    };

    assert(sym < 704);
    sym = map[sym >> 6] + (sym & 7);
    return (size_t)base[sym] + bits(s, extra[sym]);
}

/*
 * Return the context ID in 0..63, given the last two bytes and the mode.
 */
local unsigned context_id(unsigned p1, unsigned p2, unsigned mode)
{
    unsigned char const lut0[] = {
        0,  0,  0,  0,  0,  0,  0,  0,  0,  4,  4,  0,  0,  4,  0,  0, 0,  0,
        0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 8, 12, 16, 12,
        12, 20, 12, 16, 24, 28, 12, 12, 32, 12, 36, 12, 44, 44, 44, 44, 44, 44,
        44, 44, 44, 44, 32, 32, 24, 40, 28, 12, 12, 48, 52, 52, 52, 48, 52, 52,
        52, 48, 52, 52, 52, 52, 52, 48, 52, 52, 52, 52, 52, 48, 52, 52, 52, 52,
        52, 24, 12, 28, 12, 12, 12, 56, 60, 60, 60, 56, 60, 60, 60, 56, 60, 60,
        60, 60, 60, 56, 60, 60, 60, 60, 60, 56, 60, 60, 60, 60, 60, 24, 12, 28,
        12,  0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
        1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
        1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 3, 2, 3, 2,
        3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2,
        3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2,
        3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3
    };
    unsigned char const lut1[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1,
        1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
        3, 3, 3, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2
    };
    unsigned char const lut2[] = {
        0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
        3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
        5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
        6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7
    };

    assert(p1 < 256 && p2 < 256);
    switch (mode) {
        case 0:         /* LSB6 */
            return p1 & 0x3f;
        case 1:         /* MSB6 */
            return p1 >> 2;
        case 2:         /* UTF8 */
            return lut0[p1] | lut1[p2];
        case 3:         /* Signed */
            return (lut2[p1] << 3) | lut2[p2];
        default:
            assert(0);
    }
}

/*
 * Get a distance given the distance symbol.  Do not update ring buffer if the
 * distance is greater than max.
 */
local size_t distance(state_t *s, unsigned sym, size_t max)
{
    size_t dist, off;
    unsigned n, x;
    unsigned char const back[] = {
        0, 1, 2, 3, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1
    };
    signed char const delta[] = {
        0, 0, 0, 0, -1, 1, -2, 2, -3, 3, -1, 1, -2, 2, -3, 3
    };

    if (sym < 16)
        dist = s->ring[(s->ring_ptr - back[sym]) & 3] + delta[sym];
    else if (sym < 16 + s->direct)
        dist = sym - 15;
    else {
        n = sym - s->direct - 16;
        x = 1 + (n >> (s->postfix + 1));
        off = ((2 + ((n >> s->postfix) & 1)) << x) - 4;
        dist = ((off + bits(s, x)) << s->postfix) +
               (n & ((1U << s->postfix) - 1)) +
               s->direct + 1;
    }
    if (sym && dist <= max) {
        s->ring_ptr = (s->ring_ptr + 1) & 3;
        s->ring[s->ring_ptr] = dist;
    }
    return dist;
}

/*
 * Decompress one meta-block.  Return true if this is the last meta-block.
 */
local unsigned metablock(state_t *s)
{
    unsigned last;              /* true if this is the last meta-block */
    size_t mlen;                /* number of uncompressed bytes */
    unsigned dists;             /* number of distance codes */
    unsigned iac_sym;           /* insert and copy symbol */
    size_t insert;              /* insertion length */
    size_t copy;                /* copy length */
    size_t dist;                /* copy distance */
    size_t max;                 /* maximum distance within sliding window */
    unsigned p1, p2;            /* last and second-to-last output bytes */
    unsigned n;

    /* read and process the meta-block header */

    /* get last marker, and check for empty block if last */
    last = bits(s, 1);                                  /* ISLAST */
    if (last) {
        trace("last meta-block");
        if (bits(s, 1)) {                               /* ISEMPTY */
            trace("empty meta-block");
            return last;
        }
    }

    /* get the number of bytes to decompress in meta-block */
    n = bits(s, 2);                                     /* MNIBBLES - 4 */
    mlen = 1 + bits(s, 16);                             /* MLEN low 4 nybs */
    if (n) {
        mlen += bits(s, n << 2) << 16;                  /* MLEN remainder */
        if ((mlen >> ((n + 3) << 2)) == 0)
            throw(3, "more meta-block length nybbles than needed");
    }
    trace("%zu byte%s to uncompress", PLURAL(mlen));
    s->dest = alloc(s->dest, s->got + mlen);

    /* check for and process uncompressed data */
    if (!last && bits(s, 1)) {                          /* ISUNCOMPRESSED */
        /* discard any leftover bits to go to byte boundary */
        if (s->left && s->bits)
            throw(3, "discarded bits before uncompressed data not zero");
        s->bits = 0;
        s->left = 0;

        /* check that enough input is available for mlen bytes */
        if (mlen > s->len)
            throw(2, "premature end of input");

        /* write uncompressed data */
        memcpy(s->dest + s->got, s->next, mlen);
        s->got += mlen;
        s->next += mlen;
        s->len -= mlen;
        trace("stored block");

        /* return false (this isn't the last meta-block) */
        return last;
    }

    /* get the literal type and count codes */
    s->lit_prev = 0;
    s->lit_last = 1;
    s->lit_type = 0;
    s->lit_num = block_types(s);                        /* NBLTYPESL */
    trace("%u literal code type%s", PLURAL(s->lit_num));
    if (s->lit_num > 1) {
        prefix(s, &s->lit_types, s->lit_num + 2);       /* HTREE_BTYPE_L */
        prefix(s, &s->lit_count, BLOCK_LENGTH_CODES);   /* HTREE_BLEN_L */
        s->lit_left = block_length(s, &s->lit_count);   /* BLEN_L */
        trace("%zu literal%s of the first type", PLURAL(s->lit_left));
    }
    else
        s->lit_left = (size_t)0 - 1;

    /* get the insert and copy type and count codes */
    s->iac_prev = 0;
    s->iac_last = 1;
    s->iac_type = 0;
    s->iac_num = block_types(s);                        /* NBLTYPESI */
    trace("%u insert code type%s", PLURAL(s->iac_num));
    if (s->iac_num > 1) {
        prefix(s, &s->iac_types, s->iac_num + 2);       /* HTREE_BTYPE_I */
        prefix(s, &s->iac_count, BLOCK_LENGTH_CODES);   /* HTREE_BLEN_I */
        s->iac_left = block_length(s, &s->iac_count);   /* BLEN_I */
        trace("%zu insert%s of the first type", PLURAL(s->iac_left));
    }
    else
        s->iac_left = (size_t)0 - 1;

    /* get the distance type and count codes */
    s->dist_prev = 0;
    s->dist_last = 1;
    s->dist_type = 0;
    s->dist_num = block_types(s);                       /* NBLTYPESD */
    trace("%u distance code type%s", PLURAL(s->dist_num));
    if (s->dist_num > 1) {
        prefix(s, &s->dist_types, s->dist_num + 2);     /* HTREE_BTYPE_D */
        prefix(s, &s->dist_count, BLOCK_LENGTH_CODES);  /* HTREE_BLEN_I */
        s->dist_left = block_length(s, &s->dist_count); /* BLEN_D */
        trace("%zu distance%s of the first type", PLURAL(s->dist_left));
    }
    else
        s->dist_left = (size_t)0 - 1;

    /* get number of direct distance codes */
    s->postfix = bits(s, 2);                            /* NPOSTFIX */
    s->direct = bits(s, 4) << s->postfix;               /* NDIRECT */
    dists = 16 + s->direct + (48 << s->postfix);
    trace("%u direct distance codes (%u total)", s->direct, dists);

    /* get the context modes for each literal type */
    trace("%u literal type context mode%s", PLURAL(s->lit_num));
    for (n = 0; n < s->lit_num; n++)
        s->mode[n] = bits(s, 2);                        /* CMODE */

    /* get the number of literal prefix codes and literal context map */
    s->lit_codes = block_types(s);                      /* NTREESL */
    trace("%u literal code%s", PLURAL(s->lit_codes));
    if (s->lit_codes > 1)                               /* CMAPL */
        context_map(s, s->lit_map, s->lit_num << 6, s->lit_codes);

    /* get the number of distance prefix codes and distance context map */
    s->dist_codes = block_types(s);                     /* NTREESD */
    trace("%u distance code%s", PLURAL(s->dist_codes));
    if (s->dist_codes > 1)                              /* CMAPD */
        context_map(s, s->dist_map, s->dist_num << 2, s->dist_codes);

    /* get lit_codes literal prefix codes */
    trace("%u literal prefix code%s:", PLURAL(s->lit_codes));
    s->lit_code = alloc(NULL, s->lit_codes * sizeof(prefix_t));
    for (n = 0; n < s->lit_codes; n++)
        prefix(s, s->lit_code + n, 256);                /* HTREEL[n] */

    /* get iac_num insert and copy prefix codes */
    trace("%u insert and copy prefix code%s:", PLURAL(s->iac_num));
    s->iac_code = alloc(NULL, s->iac_num * sizeof(prefix_t));
    for (n = 0; n < s->iac_num; n++)
        prefix(s, s->iac_code + n, 704);                /* HTREEI[n] */

    /* get dist_codes distance prefix codes */
    trace("%u distance prefix code%s:", PLURAL(s->dist_codes));
    s->dist_code = alloc(NULL, s->dist_codes * sizeof(prefix_t));
    for (n = 0; n < s->dist_codes; n++)
        prefix(s, s->dist_code + n, dists);             /* HTREED[n] */

    /* done with header */

    /* decode the meta-block data */
    do {
        /* get insert and copy lengths */
        if (s->iac_left == 0) {
            /* change to a new insert and copy type */
            n = decode(s, &s->iac_types);
            n = n > 1 ? n - 2 :
                n ? (s->iac_last + 1) % s->iac_num :
                s->iac_prev;
            s->iac_prev = s->iac_last;
            s->iac_last = s->iac_type;
            s->iac_type = n;
            s->iac_left = block_length(s, &s->iac_count);
            assert(s->iac_left > 0);
        }
        s->iac_left--;
        iac_sym = decode(s, s->iac_code + s->iac_type);
        insert = insert_length(s, iac_sym);
        copy = copy_length(s, iac_sym);

        /* insert literals */
        trace("insert %zu literal%s", PLURAL(insert));
        if (insert > mlen)
            throw(3, "mlen exceeded by insert length");
        mlen -= insert;
        while (insert) {
            if (s->lit_left == 0) {
                /* change to a new literal type */
                n = decode(s, &s->lit_types);
                n = n > 1 ? n - 2 :
                    n ? (s->lit_last + 1) % s->lit_num :
                    s->lit_prev;
                s->lit_prev = s->lit_last;
                s->lit_last = s->lit_type;
                s->lit_type = n;
                s->lit_left = block_length(s, &s->lit_count);
                assert(s->lit_left > 0);
            }
            s->lit_left--;
            if (s->lit_codes > 1) {
                p1 = s->got ? s->dest[s->got - 1] : 0;
                p2 = s->got > 1 ? s->dest[s->got - 2] : 0;
                n = context_id(p1, p2, s->mode[s->lit_type]);
                n = s->lit_map[(s->lit_type << 6) + n];
                assert(n < s->lit_codes);
            }
            else
                n = 0;
            s->dest[s->got++] = decode(s, s->lit_code + n);
            insert--;
        }

        /* if reached mlen, then done (ignore copy length, even though it's not
           zero) */
        if (mlen == 0)
            break;

        /* get the copy distance */
        max = s->got > s->wsize ? s->wsize : s->got;
        if (iac_sym < 128)
            /* use the last distance */
            dist = s->ring[s->ring_ptr];
        else {
            /* get the distance from the stream */
            if (s->dist_left == 0) {
                /* change to a new distance type */
                n = decode(s, &s->dist_types);
                n = n > 1 ? n - 2 :
                    n ? (s->dist_last + 1) % s->dist_num :
                    s->dist_prev;
                s->dist_prev = s->dist_last;
                s->dist_last = s->dist_type;
                s->dist_type = n;
                s->dist_left = block_length(s, &s->dist_count);
                assert(s->dist_left > 0);
            }
            s->dist_left--;
            if (s->dist_codes > 1) {
                n = copy > 4 ? 3 : copy - 2;
                n = s->dist_map[(s->dist_type << 2) + n];
                assert(n < s->dist_codes);
            }
            else
                n = 0;
            dist = distance(s, decode(s, s->dist_code + n), max);
            trace("copy %zu byte%s from distance %zu", PLURAL(copy), dist);
        }

        /* copy */
        if (copy > mlen)
            throw(3, "mlen exceeded by copy length");
        mlen -= copy;
        if (dist > max) {
            /* dictionary copy */
            trace("dictionary copy");
            s->got += copy;     /* %% for now */
        }
        else {
            do {
                s->dest[s->got] = s->dest[s->got - dist];
                s->got++;
            } while (--copy);
        }
   } while (mlen);

    /* return true if this is the last meta-block */
    return last;
}

/*
 * Create a new brotli decoder state.  The brotli stream is comp[0..len-1].
 * The distances ring buffer is only initialized at the start of the stream
 * (not at the start of each meta-block).
 */
local state_t *new_state(void const *comp, size_t len)
{
    state_t *s;

    s = alloc(NULL, sizeof(state_t));
    s->next = comp;
    s->len = len;
    s->bits = 0;
    s->left = 0;
    s->dest = NULL;
    s->got = 0;
    s->ring[0] = 16;
    s->ring[1] = 15;
    s->ring[2] = 11;
    s->ring[3] = 4;
    s->ring_ptr = 3;
    s->lit_code = NULL;
    s->iac_code = NULL;
    s->dist_code = NULL;
    return s;
}

/*
 * Release the resources used by the brotli decoder state.
 */
local void free_state(state_t *s)
{
    free(s->dest);
    free(s->lit_code);
    free(s->iac_code);
    free(s->dist_code);
    free(s);
}

/*
 * Decompress.  See yeast.h for description.
 */
int yeast(void const *source, size_t *len, void **dest, size_t *got)
{
    state_t *s = NULL;
    ball_t err;

    try {
        /* initialize the decoding state */
        s = new_state(source, *len);
    }
    preserve {
        /* get the sliding window size */
        s->wbits = bits(s, 1) ? bits(s, 3) + 17 : 16;  /* WBITS */
        s->wsize = ((uint32_t)1 << s->wbits) - 16;
        trace("window size = %" PRIu32 " (%u bits)", s->wsize, s->wbits);

        /* decompress meta-blocks until last block */
        while (metablock(s) == 0)
            ;
        trace("%zu(%u) bytes(bits) unused", s->len, s->left);
    }
    always {
        *len -= s->len;
        *dest = s->dest;
        *got = s->got;
        s->dest = NULL;
        free_state(s);
    }
    catch (err) {
        trace("error: %s -- aborting", err.why);
        drop(err);
    }
    return err.code;
}
