/* Compute a 32-bit or 64-bit xxhash on the data from stdin.  If there is a
   numeric argument, then the absolute value of the argument is the number of
   times to repeat the calculation for speed testing, and the sign of the
   argument determines the length of the hash -- if negative, compute the
   64-bit hash, if positive, compute the 32-bit hash.  All of the input is
   loaded into memory for speed testing of the hash implementations. */

#include <stdio.h>
#include <stdlib.h>
#include "load.h"
#include "xxhash.h"

int main(int argc, char **argv)
{
    // interpret the argument
    if (argc > 2) {
        fputs("only one argument permitted\n", stderr);
        return 1;
    }
    long rep = argc == 1 ? 1 : strtol(argv[1], NULL, 10);
    if (rep == 0) {
        fputs("usage: xxh [[-]nnn] < data\n"
              "  where nnn is the number of times to repeat\n"
              "  negative: 64-bit check, positive: 32-bit check\n", stderr);
        return 0;
    }
    int do64 = rep < 0;
    if (do64)
        rep = -rep;

    // load the input data
    void *dat = NULL;
    size_t len, size;
    int ret = load(stdin, 0, &dat, &size, &len);
    if (ret) {
        fprintf(stderr, "load() returned %d\n", ret);
        fputs("error reading from stdin\n", stderr);
        return 1;
    }

    // compute and display the hash
    if (do64) {
        XXH64_state_t *s = XXH64_createState();
        do {
            XXH64_reset(s, 0);
            XXH64_update (s, dat, len);
        } while (--rep);
        printf("0x%16llx\n", XXH64_digest(s));
        XXH64_freeState(s);
    }
    else {
        XXH32_state_t *s = XXH32_createState();
        do {
            XXH32_reset(s, 0);
            XXH32_update (s, dat, len);
        } while (--rep);
        printf("0x%08x\n", XXH32_digest(s));
        XXH32_freeState(s);
    }

    // clean up
    free(dat);
    return 0;
}
