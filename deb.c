/* Decompress brotli streams on the command line or from stdin using yeast.
   The compressed output is written to the same name with the suffix ".bro" or
   ".compressed" removed and ".out" added, or to "deb.out" when reading from
   stdin. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "yeast.h"

#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(__CYGWIN__)
#  include <fcntl.h>
#  include <io.h>
#  define SET_BINARY_MODE(file) setmode(fileno(file), O_BINARY)
#else
#  define SET_BINARY_MODE(file)
#endif

/* Load the entire file into a memory buffer.  load() returns 0 on success, in
   which case it puts all of the file data in *dat[0..*len - 1].  That is,
   unless *len is zero, in which case *dat is NULL.  *dat is allocated memory
   which should be freed when done with it.  load() returns zero on success,
   with *dat == NULL and *len == 0.  The error values are -1 for read error or
   1 for out of memory.  To guard against bogging down the system with
   extremely large allocations, if limit is not zero then load() will return an
   out of memory error if the input is larger than limit. */
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

#define SUFFIX1 ".compressed"
#define SUFFIX2 ".bro"
#define OUT ".out"

/* Write decompressed output to a derived file name with extension ".out". */
static void deliver(char *in, void *data, size_t len)
{
    char *out;
    FILE *file;
    size_t inlen, suf;

    inlen = strlen(in);
    out = malloc(inlen + strlen(OUT) + 1);
    if (out == NULL) {
        fprintf(stderr, "out of memory");
        return;
    }
    suf = strlen(SUFFIX1);
    if (inlen >= suf && strcmp(in + inlen - suf, SUFFIX1) == 0)
        inlen -= suf;
    else {
        suf = strlen(SUFFIX2);
        if (inlen >= suf && strcmp(in + inlen - suf, SUFFIX2) == 0)
            inlen -= suf;
    }
    memcpy(out, in, inlen);
    strcpy(out + inlen, OUT);
    file = fopen(out, "wb");
    if (file == NULL) {
        fprintf(stderr, "could not create %s", out);
        free(out);
        return;
    }
    fwrite(data, 1, len, file);
    fclose(file);
    free(out);
}

/* Decompress all of the files on the command line, or from stdin if no
   arguments. */
int main(int argc, char **argv)
{
    FILE *in;
    int ret;
    unsigned char *source;
    void *dest;
    size_t len, got;

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
                fprintf(stderr, "deb: invalid option %s\n", opt);
                return 1;
            }
        }
    }
#endif

    /* decompress each file on the remaining command line */
    if (--argc) {
        for (;;) {
            in = fopen(*++argv, "rb");
            if (in == NULL) {
                fprintf(stderr, "error opening %s\n", *argv);
                continue;
            }
            ret = load(in, &source, &len, 0);
            fclose(in);
            if (ret < 0) {
                fprintf(stderr, "error reading %s\n", *argv);
                continue;
            }
            if (ret > 0) {
                fputs("out of memory\n", stderr);
                return 1;
            }
            fputs(*argv, stderr);
            fputs(":\n", stderr);
            ret = yeast(&dest, &got, source, &len, 0);
            fprintf(stderr, "uncompressed length = %zu\n", got);
            if (ret)
                fprintf(stderr, "yeast() returned %d\n", ret);
            deliver(*argv, dest, got);
            free(dest);
            free(source);
            if (--argc == 0)
                break;
            putc('\n', stderr);
        }
    }

    /* or if no names on the remaining command line, decompress from stdin */
    else {
        SET_BINARY_MODE(stdin);
        ret = load(stdin, &source, &len, 0);
        if (ret) {
            fputs(ret > 0 ? "out of memory\n" : "error reading stdin\n",
                  stderr);
            return 1;
        }
        ret = yeast(&dest, &got, source, &len, 0);
        fprintf(stderr, "uncompressed length = %zu\n", got);
        if (ret)
            fprintf(stderr, "yeast() returned %d\n", ret);
        deliver("deb", dest, got);
        free(dest);
        free(source);
    }
    return 0;
}
