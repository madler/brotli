/*
 * iltorb.c
 * Copyright (C) 2015 Mark Adler
 * For conditions of distribution and use, see the accompanying LICENSE file.
 *
 * iltorb.c is a simple decompressor of the brotli format, written to both test
 * the completeness and correctness of the brotli specification, and once
 * verified, to provide an unambiguous specification of the format by virtue of
 * being a working decoder.  It is a higher priority for this code be simple
 * and readable than to be fast.
 *
 * This code is intended to be compliant with the C99 standard.
 *
 */

#include <stdlib.h>
#include <stddef.h>
#include <inttypes.h>
#include <assert.h>
#include "try.h"
#include "iltorb.h"

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
        fputs("iltorb: ", stderr); \
        fprintf(stderr, __VA_ARGS__); \
        putc('\n', stderr); \
    } while (0)
#else
#  define trace(...)
#endif

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
 * Brotli decoding state.
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

    /* codes */
    prefix_t *lit_code;             /* literal codes (allocated) */
    prefix_t *iac_code;             /* insert codes (allocated) */
    prefix_t *dist_code;            /* distance codes (allocated) */
    prefix_t lit_types;             /* literal block types */
    prefix_t lit_count;             /* literal block lengths */
    prefix_t iac_types;             /* insert and copy block types */
    prefix_t iac_count;             /* insert and copy block lengths */
    prefix_t dist_types;            /* distance block types */
    prefix_t dist_count;            /* distance block lengths */
} state_t;

/*
 * Write out compressed data.  (Currently no-op.)
 */
local void deliver(state_t *s, unsigned char const *data, size_t len)
{
    (void)s;
    (void)data;
    (void)len;
}

/*
 * Return need bits from the input stream.  need must be in 0..57.  This will
 * leave 0..7 bits in s->bits.
 *
 * Format notes:
 *
 * - Bits are stored in bytes from the least significant bit to the most
 *   significant bit.  Therefore bits are dropped from the bottom of the bit
 *   buffer, using shift right, and new bytes are appended to the top of the
 *   bit buffer, using shift left.
 *
 * - Up to 28 bits may be requested (for a macro-block length).
 */
local uint64_t bits(state_t *s, unsigned need)
{
    uint64_t reg;       /* register in which to accumulate need bits */

    assert(need < 58);
    reg = s->bits;
    while (s->left < need) {
        if (s->len == 0)
            throw(2, "premature end of input");
        reg |= (uint64_t)(*(s->next)++) << s->left;
        s->len--;
        s->left += 8;
    }
    s->bits = reg >> need;
    s->left -= need;
    return reg & (((uint64_t)1 << need) - 1);
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
local unsigned decode(state_t *s, const prefix_t *p)
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
 * n symbols, construct the tables required to decode those codes. Those tables
 * are the number of codes of each length, and the symbols sorted by length,
 * and sorted by symbol value within each length.  construct() assumes that the
 * provided lengths consititute a complete prefix code.
 *
 * length[k] == 0 means that symbol k is not coded.  Otherwise length[k] is the
 * number of bits used for symbol k.
 *
 * Format notes:
 *
 * - The brotli format only permits complete codes.
 *
 * - The brotli format permits codes with a single symbol whose code is zero
 *   bits.  This function is not called in that case, nor is it called for the
 *   other simple prefix codes since the symbols are provided differently in
 *   that descriptor.  For those, simple() is called instead.
 */
local void construct(prefix_t *p, unsigned short const *length, unsigned n)
{
    unsigned symbol;                /* current symbol */
    unsigned len;                   /* current length */
    unsigned slen;                  /* number of bits for this symbol */
    unsigned short offs[MAXBITS+1]; /* symbol offsets for each length */

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

/* Define SORTSIMPLE if the symbols provided in simple codes should be sorted
   within bit lengths to assure a canonical code.  Define SORTCHECK for ORDER
   to verify that the symbols are already sorted and therefore that the
   generated code is canonical.  Otherwise, do not check or reorder the
   symbols. */
#if defined(SORTSIMPLE)
#  define ORDER(list, i, j) \
    do { \
        if (list[i] > list[j]) { \
            unsigned short tmp = list[i]; \
            list[i] = list[j]; \
            list[j] = tmp; \
        } \
    } while (0)
#elif defined(SORTCHECK)
#  define ORDER(list, i, j) \
    do { \
        if (list[i] >= list[j]) \
            throw(3, "simple code symbols not sorted"); \
    } while (0)
#else
#  define ORDER(...)
#endif

/*
 * Construct the tables required to decode the provided simple prefix code.
 * type is 1 for one symbol of zero length; 2 for two symbols each of length 1;
 * 3 for three symbols of code lengths of 1, 2, 2; 4 for four symbols of code
 * lengths 2, 2, 2, 2; and 5 for four symbols of code lengths 1, 2, 3, 3.
 *
 * Format note:
 * - The symbols provided in the stream for the same bit length might not be in
 *   sorted order.  In fact, the example file asyoulike.txt.compressed has such
 *   a simple code for the distance block length code, where symbol 21 with
 *   code length 3 precedes symbol 20 with code length 3.  So not all prefix
 *   codes used in brotli are canonical (as claimed in verison 02 of the
 *   specification).  Therefore, neither SORTSIMPLE nor SORTCHECK should be
 *   #defined.
 */
local void simple(prefix_t *p, unsigned short const *syms, unsigned type)
{
    unsigned n;

    for (n = 0; n <= MAXBITS; n++)
        p->count[n] = 0;
    switch (type) {
        case 1:
            p->count[0] = 1;
            break;
        case 2:
            p->count[1] = 2;
            ORDER(syms, 0, 1);
            break;
        case 3:
            p->count[1] = 1;
            p->count[2] = 2;
            ORDER(syms, 1, 2);
            break;
        case 4:
            p->count[2] = 4;
            ORDER(syms, 0, 1);
            ORDER(syms, 2, 3);
#ifdef SORTSIMPLE
            ORDER(syms, 0, 2);
            ORDER(syms, 1, 3);
#endif
            ORDER(syms, 1, 2);
            break;
        case 5:
            p->count[1] = 1;
            p->count[2] = 1;
            p->count[3] = 2;
            ORDER(syms, 2, 3);
            type--;
            break;
        default:
            assert(0);
    }
    for (n = 0; n < type; n++)
        p->symbol[n] = syms[n];
}

/*
 * Read in a prefix code description and save the tables in p.  max is the
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
        unsigned n;
        unsigned abits;             /* alphabet bits */
        unsigned sym;               /* symbol */
        unsigned short syms[4];     /* symbols for this code */

        trace("simple prefix code");

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
            syms[n] = sym < num ? sym : sym - num;
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
        unsigned last;          /* last len */
        unsigned rep;           /* number of times to repeat last len */
        unsigned zeros;         /* number of times to repeat zero */
        unsigned n, k;

        /* initially the code for code length code lengths, then reused for
           code lengths */
        prefix_t code = {{0, 0, 3, 1, 2}, {0, 3, 4, 2, 1, 5}};

        /* order of code length code lengths */
        unsigned short const order[] = {
            1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15
        };

        /* lengths read for code lengths code, then reused for code */
        unsigned short lens[num < 18 ? 18 : num];

        trace("complex prefix code");

        /* read code length code lengths and make code length code */
        left = 1 << 5;
        for (n = 0; n < hskip; n++)
            lens[order[n]] = 0;
        for (; n < 18; n++) {
            len = decode(s, &code);
            lens[order[n]] = len;
            if (len) {
                left -= (1 << 5) >> len;
                if (left <= 0) {
                    n++;
                    break;
                }
            }
        }
        if (left < 0)
            throw(3, "oversubscribed code length code");
        if (left)
            throw(3, "incomplete code length code");
        for (; n < 18; n++)
            lens[order[n]] = 0;
        construct(&code, lens, 18);

        /* read code lengths and make code */
        left = 1U << MAXBITS;
        last = 8;
        rep = 0;
        zeros = 0;
        nsym = 0;
        do {
            len = decode(s, &code);
            if (len < 16) {
                if (nsym == num)
                    throw(3, "too many symbols");
                lens[nsym++] = len;
                if (len) {
                    left -= (1U << MAXBITS) >> len;
                    last = len;
                }
                rep = 0;
                zeros = 0;
            }
            else if (len == 16) {
                rep = rep ? 3 * rep - 5 : 3;
                rep += bits(s, 2);
                for (k = 0; k < rep; k++) {
                    if (nsym == num)
                        throw(3, "too many symbols");
                    lens[nsym++] = last;
                    left -= (1 << 15) >> last;
                }
                zeros = 0;
            }
            else {  /* len == 17 */
                zeros = zeros ? 7 * zeros - 13 : 3;
                zeros += bits(s, 3);
                for (k = 0; k < zeros; k++) {
                    if (nsym == num)
                        throw(3, "too many symbols");
                    lens[nsym++] = 0;
                }
                rep = 0;
            }
        } while (left > 0);
        if (left < 0)
            throw(3, "oversubscribed code");
        construct(p, lens, nsym);
    }

#ifdef DEBUG
    /* show the prefix code */
    {
        unsigned n, k, i;

        i = 0;
        for (n = 0; n <= MAXBITS; n++)
            for (k = 0; k < p->count[n]; k++)
                trace("  %u: %u", n, p->symbol[i++]);
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
    trace("block length symbol %u", sym);
    assert(sym < BLOCK_LENGTH_CODES);
    return (size_t)base[sym] + bits(s, extra[sym]);
}

/*
 * Decode the number of block types.
 *
 * Format note: Version 02 of the brotli specification is rather misleading on
 * - how this is coded.  The "variable length code" is actually a leading 0 or
 *   1, followed by a three-bit integer (not a reversed code) which is the
 *   number of extra bits as well as a value from which the base can be readily
 *   computed.  That is followed by the extra bits, not reversed.
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
 * Decompress one meta-block.  Return true if this is the last meta-block.
 */
local unsigned metablock(state_t *s)
{
    unsigned last;              /* true if this is the last meta-block */
    unsigned nybs;              /* number of nybbles in the length */
    unsigned types;             /* number of types of each code */
    size_t mlen;                /* number of uncompressed bytes */

    /* get last marker, and check for empty block if last */
    last = bits(s, 1);
    if (last) {
        trace("last meta-block");
        if (bits(s, 1)) {
            trace("empty meta-block");
            return last;
        }
    }

    /* get number of bytes to decompress in meta-block */
    nybs = bits(s, 2) + 4;
    mlen = bits(s, nybs << 2) + 1;
    if (nybs > 4 && (mlen >> ((nybs - 1) << 2)) == 0)
        throw(3, "more meta-block length nybbles than needed");
    trace("%zu uncompressed byte%s", mlen, mlen == 1 ? "" : "s");

    /* check for and process uncompressed data */
    if (!last && bits(s, 1)) {
        /* discard any leftover bits to go to byte boundary */
        s->bits = 0;
        s->left = 0;

        /* check that enough input is available for mlen bytes */
        if (mlen > s->len)
            throw(2, "premature end of input");

        /* write uncompressed data and update input */
        deliver(s, s->next, mlen);
        s->next += mlen;
        s->len -= mlen;
        trace("stored block");

        /* return false (this isn't the last meta-block) */
        return last;
    }

    /* get literal type and count codes */
    s->lit_prev = 0;
    s->lit_last = 1;
    s->lit_type = 0;
    s->lit_num = types = block_types(s);
    trace("%u literal code type%s", types, types == 1 ? "" : "s");
    if (types > 1) {
        prefix(s, &s->lit_types, types + 2);
        prefix(s, &s->lit_count, BLOCK_LENGTH_CODES);
        s->lit_left = block_length(s, &s->lit_count);
        trace("%zu literal%s of the first type",
              s->lit_left, s->lit_left == 1 ? "" : "s");
    }
    else
        s->lit_left = (size_t)0 - 1;

    /* get insert and copy type and count codes */
    s->iac_prev = 0;
    s->iac_last = 1;
    s->iac_type = 0;
    s->iac_num = types = block_types(s);
    trace("%u insert code type%s", types, types == 1 ? "" : "s");
    if (types > 1) {
        prefix(s, &s->iac_types, types + 2);
        prefix(s, &s->iac_count, BLOCK_LENGTH_CODES);
        s->iac_left = block_length(s, &s->iac_count);
        trace("%zu insert%s of the first type",
              s->iac_left, s->iac_left == 1 ? "" : "s");
    }
    else
        s->iac_left = (size_t)0 - 1;

    /* get distance type and count codes */
    s->dist_prev = 0;
    s->dist_last = 1;
    s->dist_type = 0;
    s->dist_num = types = block_types(s);
    trace("%u distance code type%s", types, types == 1 ? "" : "s");
    if (types > 1) {
        prefix(s, &s->dist_types, types + 2);
        prefix(s, &s->dist_count, BLOCK_LENGTH_CODES);
        s->dist_left = block_length(s, &s->dist_count);
        trace("%zu distance%s of the first type",
              s->dist_left, s->dist_left == 1 ? "" : "s");
    }
    else
        s->dist_left = (size_t)0 - 1;

    /* return true if this is the last meta-block */
    return 1 /* %% should be return last -- stop here for now */;
}

/*
 * Initialize brotli state.
 */
local void init_state(state_t *s, void const *comp, size_t len)
{
    s->next = comp;
    s->len = len;
    s->bits = 0;
    s->left = 0;
    s->lit_code = NULL;
    s->iac_code = NULL;
    s->dist_code = NULL;
}

/*
 * Release allocated memory in brotli state.
 */
local void free_state(state_t *s)
{
    free(s->lit_code);
    free(s->iac_code);
    free(s->dist_code);
    s->lit_code = NULL;
    s->iac_code = NULL;
    s->dist_code = NULL;
}

/*
 * Decompress the compressed brolti stream: comp[0..len-1].
 */
int iltorb(void const *comp, size_t len)
{
    state_t s;
    ball_t err;

    try {
        /* initialize the decoding state */
        init_state(&s, comp, len);

        /* get the sliding window size */
        s.wbits = bits(&s, 1) ? bits(&s, 3) + 17 : 16;
        s.wsize = ((uint32_t)1 << s.wbits) - 16;
        trace("window size = %" PRIu32 " (%u bits)", s.wsize, s.wbits);

        /* decompress meta-blocks until last block */
        while (metablock(&s) == 0)
            ;
        trace("%zu(%u) bytes(bits) unused", s.len, s.left);
    }
    always {
        free_state(&s);
    }
    catch (err) {
        trace("error: %s -- aborting", err.why);
        drop(err);
    }
    return err.code;
}
