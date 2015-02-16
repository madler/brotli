/* Show offsets and number of dictionary words, read in the dictionary from
   stdin, and show all the words therein. */

#include <stdio.h>
#include <assert.h>

/* Log base 2 of the number of words of each length, for lengths from 4 to 24.
   (Ignore array values for lengths 0..3.) */
static const unsigned ndbits[] = {
    0, 0, 0, 0, 10, 10, 11, 11, 10, 10, 10, 10, 10, 9, 9, 8, 7, 7, 8, 7, 7, 6,
    6, 5, 5
};

/* Show one character in a word, honoring UTF-8 when a legal UTF-8 character is
   presented, and converting control and other non-printable characters to
   escape codes.  The number of bytes consumed in str is returned.  No more
   than len will be consumed.  (This does not reject all overlong UTF-8
   encodings.) */
static size_t show_char(unsigned char *str, size_t len)
{
    size_t used, n;

    used = 1;
    if (*str == '\\')
        fputs("\\\\", stdout);
    else if (*str >= ' ' && *str < 0x7f)
        putchar(*str);
    else if (*str == '\t')
        fputs("\\t", stdout);
    else if (*str == '\n')
        fputs("\\n", stdout);
    else if (*str == '\r')
        fputs("\\r", stdout);
    else if (*str < ' ' || (*str >= 0x7f && *str < 0xc2) || *str >= 0xf5)
        printf("\\x%02x", *str);
    else {
        used = *str < 0xe0 ? 2 : *str < 0xf0 ? 3 : 4;
        for (n = 1; n < used && n < len; n++)
            if (str[n] < 0x80 || str[n] >= 0xc0)
                break;
        if (n == used)
            fwrite(str, 1, used, stdout);
        else {
            printf("\\x%02x", *str);
            used = 1;
        }
    }
    return used;
}

/* Show the word of length len at *str. */
static void show_word(unsigned char *str, size_t len)
{
    size_t used;

    while (len) {
        used = show_char(str, len);
        str += used;
        len -= used;
    }
}

/* Read the dictionary from stdin and show all the words in it. */
static void show_dict(size_t tot)
{
    size_t got, len, num;
    unsigned char dict[tot], *next;

    got = fread(dict, 1, tot, stdin);
    assert(got == tot && getchar() == EOF);
    next = dict;
    for (len = 4; len <= 24; len++) {
        num = 1 << ndbits[len];
        printf("\nlength %lu words (%lu):\n", len, num);
        do {
            fputs("    ", stdout);
            show_word(next, len);
            fputs("    \n", stdout);    /* more indenting for Arabic! */
            next += len;
        } while (--num);
    }
}

int main(void)
{
    size_t len, num, tot;

    tot = 0;
    for (len = 4; len <= 24; len++) {
        num = 1 << ndbits[len];
        printf("%lu words of length %lu at offset %lu\n",
               num, len, tot);
        tot += len * num;
    }
    printf("total dictionary size = %lu\n", tot);
    show_dict(tot);
    return 0;
}
