/* Decompress brotli streams on the command line or from stdin using yeast.
   The compressed output is written to the same name with the suffix ".bro" or
   ".compressed" removed and ".out" added, or to "deb.out" when reading from
   stdin. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "load.h"
#include "yeast.h"

#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(__CYGWIN__)
#  include <fcntl.h>
#  include <io.h>
#  define SET_BINARY_MODE(file) setmode(fileno(file), O_BINARY)
#else
#  define SET_BINARY_MODE(file)
#endif

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
    void *source = NULL;
    void *dest;
    size_t size, len, got;

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
            ret = load(in, 0, &source, &size, &len);
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
            if (--argc == 0)
                break;
            putc('\n', stderr);
        }
    }

    /* or if no names on the remaining command line, decompress from stdin */
    else {
        SET_BINARY_MODE(stdin);
        ret = load(stdin, 0, &source, &size, &len);
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
    }
    free(source);
    return 0;
}
