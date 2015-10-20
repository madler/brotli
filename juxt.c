/* Decompress a brotli stream using yeast, and compare to the associated
   original file.  The brotli stream is expected to have an extension, i.e. a
   period and characters that follow the period, and that same name with no
   extension is the associated original file.  This compares the decompressed
   bytes as they are generated, and so catches and reports the error as soon as
   possible. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "load.h"
#include "yeast.h"

/* Load the file at path into memory, allocating new memory if *dat is NULL, or
   reusing the allocation at *dat of size *size.  The length of the read data
   is returned in *len.  load_file() returns zero on success, non-zero on
   failure. */
static int load_file(char *path, void **dat, size_t *size, size_t *len)
{
    int ret = 0;
    FILE *in = fopen(path, "rb");
    if (in == NULL)
        ret = -1;
    else {
        ret = load(in, 0, dat, size, len);
        fclose(in);
    }
    if (ret)
        fprintf(stderr, "could not load %s\n", path);
    return ret;
}

/* Strip the extension off of a name in place.  Return 0 on success or -1 if
   the name does not have a period in the name or after the final slash.  If
   cut is 0, then the extension is not stripped -- only the status is returned
   in order to see if a later strip() will succeed. */
static int strip(char *path, int cut)
{
    char *dot;

    dot = strrchr(path, '.');
    if (dot == NULL || strchr(dot + 1, '/') != NULL)
        return -1;
    if (cut)
        *dot = 0;
    return 0;
}

/* Decompress and check all of the compressed files on the command line. */
int main(int argc, char **argv)
{
    int ret;
    void *compressed = NULL;
    void *uncompressed = NULL;
    size_t csize, clen, usize, ulen;

#ifdef DEBUG
    /* process verbosity option */
    if (argc > 1 && argv[1][0] == '-') {
        char *opt;

        --argc;
        opt = *++argv;
        while (*++opt) {
            if (*opt == 'v')
                yeast_verbosity++;
            else {
                fprintf(stderr, "juxt: invalid option %s\n", opt);
                return 1;
            }
        }
    }
#endif

    /* test each name in the command line remaining */
    while (++argv, --argc) {
        if (strip(*argv, 0)) {
            fprintf(stderr, "%s has no extension\n", *argv);
            continue;
        }
        if (load_file(*argv, &compressed, &csize, &clen))
            continue;
        strip(*argv, 1);
        if (load_file(*argv, &uncompressed, &usize, &ulen))
            continue;
        fprintf(stderr, "%s:\n", *argv);
        ret = yeast(&uncompressed, &ulen, compressed, &clen, 1);
        if (ret)
            fprintf(stderr, "yeast() returned %d\n", ret);
        if (argc > 1)
            putchar('\n');
    }
    free(uncompressed);
    free(compressed);
    return 0;
}
