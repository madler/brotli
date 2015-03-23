/* Determine the maximum length of the transform prefixes and suffixes. */

#include <stdio.h>
#include <string.h>

#define local static

typedef enum {
    identity,
    uppercasefirst,
    uppercaseall,
    omitfirst,
    omitlast
} xelem_t;

typedef struct {
    char *prefix;
    xelem_t xelem;
    unsigned omit;
    char *suffix;
} xform_t;

#include "xforms.h"

int main(void)
{
    size_t n, len, maxp = 0, maxs = 0;

    for (n = 0; n < sizeof(xforms) / sizeof(xform_t); n++) {
        len = strlen(xforms[n].prefix);
        if (len > maxp)
            maxp = len;
        len = strlen(xforms[n].suffix);
        if (len > maxs)
            maxs = len;
    }
    printf("%zu transforms: max prefix = %zu, max suffix = %zu\n", n, maxp, maxs);
    return 0;
}
