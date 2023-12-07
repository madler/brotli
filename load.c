#include <stdlib.h>
#include <stddef.h>
#include "load.h"

// Return true if there is more to read from in. This function is used only if
// a memory allocation or reallocation fails, or if the data fills limit.
static inline int more(FILE *in) {
    return ungetc(getc(in), in) != EOF;
}

// See load.h for description.
int load(FILE *in, size_t limit, void **dat, size_t *size, size_t *len) {
    // if no input limit, then set it to the maximum ptrdiff_t value
    if (limit == 0)
        limit = (size_t)-1 >> 1;

    // no bytes read so far
    *len = 0;

    // assure that *dat has an allocation to read into
    size_t have = size == NULL ? 0 : *size;
    if (*dat == NULL || have == 0) {
        have = (size_t)1 << 15;    // 32 KiB
        *dat = realloc(*dat, have);
        if (*dat == NULL) {
            if (size != NULL)
                *size = 0;
            return more(in) ? -2 : 0;
        }
    }

    // read from in until no more input, the limit is reached, or out of memory
    for (;;) {
        // read as much as is available into the allocated space up to limit
        size_t max = have > limit ? limit : have;
        *len += fread(*dat + *len, 1, max - *len, in);
        if (ferror(in)) {
            if (size != NULL)
                *size = have;
            return -1;
        }
        if (*len < max || have >= limit)
            break;

        // increase size to the next power of two, saturating to the maximum
        // ptrdiff_t value
        size_t reduce = have;
        size_t bigger;
        do {
            bigger = reduce;
            reduce &= reduce - 1;
        } while (reduce);
        bigger <<= 1;
        if ((ptrdiff_t)bigger < 0)
            bigger--;
        void *mem = realloc(*dat, bigger);
        if (mem == NULL) {
            if (size != NULL)
                *size = have;
            return more(in) ? -2 : 0;
        }
        *dat = mem;
        have = bigger;
    }

    // return 0 on success, 1 if limit was reached with unread input
    if (size != NULL)
        *size = have;
    return *len == limit && more(in) ? 1 : 0;
}
