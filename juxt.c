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
#include "yeast.h"

/* Load an entire file into a memory buffer.  load() returns 0 on success, in
   which case it puts all of the file data in *dat[0..*len - 1].  That is,
   unless *len is zero, in which case *dat is NULL.  *dat is allocated memory
   which should be freed when done with it.  The error values are -1 for read
   error or 1 for out of memory.  On error, *dat is NULL and *len is 0.  To
   guard against bogging down the system with extremely large allocations, if
   limit is not zero then load() will return an out of memory error if the
   input is larger than limit. */
static int load(FILE *in, unsigned char **dat, size_t *len, size_t limit)
{
    size_t size = 1048576, have = 0, was;
    unsigned char *buf = NULL, *mem;

    *dat = NULL;
    *len = 0;
    if (limit == 0)
        limit--;
    if (size >= limit)
        size = limit - 1;
    do {
        /* if we already saturated the size_t type or reached the limit, then
           out of memory */
        if (size == limit) {
            free(buf);
            return 1;
        }

        /* double size, saturating to the maximum size_t value */
        was = size;
        size <<= 1;
        if (size < was || size > limit)
            size = limit;

        /* reallocate buf to the new size */
        mem = realloc(buf, size);
        if (mem == NULL) {
            free(buf);
            return 1;
        }
        buf = mem;

        /* read as much as is available into the newly allocated space */
        have += fread(buf + have, 1, size - have, in);

        /* if we filled the space, make more space and try again until we don't
           fill the space, indicating end of file */
    } while (have == size);

    /* if there was an error reading, discard the data and return an error */
    if (ferror(in)) {
        free(buf);
        return -1;
    }

    /* if a zero-length file is read, return NULL for the data pointer */
    if (have == 0) {
        free(buf);
        return 0;
    }

    /* resize the buffer to be just big enough to hold the data */
    mem = realloc(buf, have);
    if (mem != NULL)
        buf = mem;

    /* return the data */
    *dat = buf;
    *len = have;
    return 0;
}

/* Load the file at path, returning a pointer to the data read and it's length
   in *len.  If the returned pointer is NULL and *len is not zero, then the
   load failed.  For a zero-length file, the returned pointer is NULL and *len
   is 0. */
static unsigned char *load_path (char *path, size_t *len)
{
    int ret = -1;
    unsigned char *data = NULL;
    FILE *in;

    in = fopen(path, "rb");
    if (in == NULL || (ret = load(in, &data, len, 0)) != 0) {
        if (ret < 0)
            fprintf(stderr, "error reading %s (%s)\n",
                    path, strerror(errno));
        else
            fputs("out of memory\n", stderr);
        *len = 1;
    }
    if (in != NULL)
        fclose(in);
    return data;
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
    unsigned char *compressed;
    void *uncompressed;
    size_t clen, ulen;

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
        compressed = load_path(*argv, &clen);
        if (compressed == NULL && clen)
            continue;
        strip(*argv, 1);
        uncompressed = load_path(*argv, &ulen);
        if (uncompressed == NULL && ulen) {
            free(compressed);
            continue;
        }
        fprintf(stderr, "%s:\n", *argv);
        ret = yeast(&uncompressed, &ulen, compressed, &clen, 1);
        if (ret)
            fprintf(stderr, "yeast() returned %d\n", ret);
        free(uncompressed);
        free(compressed);
        if (argc > 1)
            putchar('\n');
    }
    return 0;
}
