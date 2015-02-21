/* Show offsets and number of dictionary words, read in the dictionary from
   stdin, and show all the words therein.  Generate and show a histogram over
   the unicode character sets. */

#include <stdio.h>
#include <assert.h>

/* Log base 2 of the number of words of each length, for lengths from 4 to 24.
   (Ignore array values for lengths 0..3.) */
static const unsigned ndbits[] = {
    0, 0, 0, 0, 10, 10, 11, 11, 10, 10, 10, 10, 10, 9, 9, 8, 7, 7, 8, 7, 7, 6,
    6, 5, 5
};

/* Return the number of bytes in the UTF8 character starting at str, using no
   more than len bytes.  Return -1 if the UTF8 value is invalid.  Return 1..4
   for a UTF8 character.  Return 0 if len is 0.  This assumes that the maximum
   unicode value is U+10FFFF. */
static int utf8bytes(unsigned char *str, size_t len)
{
    if (len < 1)
        return 0;               /* empty */
    if (str[0] < 0x80)
        return 1;               /* 1-byte (ASCII) */
    if (str[0] < 0xc2)
        return -1;              /* not UTF8 or overlong 1-byte in 2-byte */
    if (len < 2 || (str[1] & 0xc0) != 0x80)
        return -1;              /* missing or invalid second byte */
    if (str[0] < 0xe0)
        return 2;               /* valid 2-byte */
    if (len < 3 || (str[2] & 0xc0) != 0x80)
        return -1;              /* missing or invalid third byte */
    if (str[0] == 0xe0 && str[1] < 0xa0)
        return -1;              /* overlong 1,2-byte in 3-byte */
    if (str[0] < 0xf0)
        return 3;               /* valid 3-byte */
    if (len < 4 || (str[3] & 0xc0) != 0x80)
        return -1;              /* missing or invalid fourth byte */
    if (str[0] == 0xf0 && str[1] < 0x90)
        return -1;              /* overlong 1,2,3-byte in 4-byte */
    if (str[0] < 0xf4 || (str[0] == 0xf4 && str[1] < 0x90))
        return 4;               /* 4-byte code <= 0x10ffff */
    return -1;                  /* code > 0x10ffff or not UTF8 */
}

/* Return the Unicode code for the UTF8 character at *str, using no more than
   len bytes.  The code returned is in 0..0x10ffff.  If there is no valid UTF8
   character, then -1 is returned.  *str is advanced by the number of bytes
   used. */
static long utf8code(unsigned char **str, size_t len)
{
    int bytes;
    long code;

    bytes = utf8bytes(*str, len);
    if (bytes < 1)
        return -1;
    code = *(*str)++ & (0xff >> bytes);
    while (--bytes)
        code = (code << 6) | (*(*str)++ & 0x3f);
    return code;
}

/* Unicode codeset ranges up to 0xffff. */
static struct range_s {
    char *name;
    long low, high;
} codeset[] = {
    {"-- invalid UTF-8 --", -1, -1},
    {"Control Character", 0, 0x1f},
    {"Basic Latin", 0x20, 0x7f},
    {"Latin1 Supplement", 0x80, 0xff},
    {"Latin Extended A", 0x100, 0x17f},
    {"Latin Extended B", 0x180, 0x24f},
    {"IPA Extensions", 0x250, 0x2af},
    {"Spacing Modifier Letters", 0x2b0, 0x2ff},
    {"Combining Diacritical Marks", 0x300, 0x36f},
    {"Greek and Coptic", 0x370, 0x3ff},
    {"Cyrillic", 0x400, 0x4ff},
    {"Cyrillic Supplement", 0x500, 0x52f},
    {"Armenian", 0x530, 0x58f},
    {"Hebrew", 0x590, 0x5ff},
    {"Arabic", 0x600, 0x6ff},
    {"Syriac", 0x700, 0x74f},
    {"Arabic Supplement", 0x750, 0x77f},
    {"Thana", 0x780, 0x7bf},
    {"NKo", 0x7c0, 0x7ff},
    {"Samaritan", 0x800, 0x83f},
    {"Mandaic", 0x840, 0x85f},
    {"-- unassigned --", 0x860, 0x89f},
    {"Arabic Extended-A", 0x8a0, 0x8ff},
    {"Devanagari", 0x900, 0x97f},
    {"Bengali", 0x980, 0x9ff},
    {"Gurmukhi", 0xa00, 0xa7f},
    {"Gujarti", 0xa80, 0xaff},
    {"Oriya", 0xb00, 0xb7f},
    {"Tamil", 0xb80, 0xbff},
    {"Telugu", 0xc00, 0xc7f},
    {"Kannada", 0xc80, 0xcff},
    {"Malayalam", 0xd00, 0xd7f},
    {"Sinhala", 0xd80, 0xdff},
    {"Thai", 0xe00, 0xe7f},
    {"Lao", 0xe80, 0xeff},
    {"Tibetan", 0xf00, 0xfff},
    {"Myanmar", 0x1000, 0x109f},
    {"Georgian", 0x10a0, 0x10ff},
    {"Hangul Jamo", 0x1100, 0x11ff},
    {"Ethiopic", 0x1200, 0x137f},
    {"Ethiopic Supplement", 0x1380, 0x139f},
    {"Cherokee", 0x13a0, 0x13ff},
    {"Unified Canadian Aboriginal Syllabics", 0x1400, 0x167f},
    {"Ogham", 0x1680, 0x169f},
    {"Runic", 0x16a0, 0x16ff},
    {"Tagalog", 0x1700, 0x171f},
    {"Hanunoo", 0x1720, 0x173f},
    {"Buhid", 0x1740, 0x175f},
    {"Tagbanwa", 0x1760, 0x177f},
    {"Khmer", 0x1780, 0x17ff},
    {"Mongolian", 0x1800, 0x18af},
    {"Unified Canadian Aboriginal Syllabics Extended", 0x18b0, 0x18ff},
    {"Limbu", 0x1900, 0x194f},
    {"Tai Le", 0x1950, 0x197f},
    {"New Tai Lue", 0x1980, 0x19df},
    {"Khmer Symbols", 0x19e0, 0x19ff},
    {"Buginese", 0x1a00, 0x1a1f},
    {"Tai Tham", 0x1a20, 0x1aaf},
    {"Combining Diacritical Marks Extended", 0x1ab0, 0x1aff},
    {"Balinese", 0x1b00, 0x1b7f},
    {"Sudanese", 0x1b80, 0x1bbf},
    {"Batak", 0x1bc0, 0x1bff},
    {"Lepcha", 0x1c00, 0x1c4f},
    {"Ol Chiki", 0x1c50, 0x1c7f},
    {"-- unassigned --", 0x1c80, 0x1cbf},
    {"Sudanese Supplement", 0x1cc0, 0x1ccf},
    {"Vedic Extensions", 0x1cd0, 0x1cff},
    {"Phonetic Extensions", 0x1d00, 0x1d7f},
    {"Phonetic Extensions Supplement", 0x1d80, 0x1dbf},
    {"Combining Diacritical Marks Supplement", 0x1dc0, 0x1dff},
    {"Latin Extended Additional", 0x1e00, 0x1eff},
    {"Greek Extended", 0x1f00, 0x1fff},
    {"General Punctuation", 0x2000, 0x206f},
    {"Superscripts and Subscripts", 0x2070, 0x209f},
    {"Currency Symbols", 0x20a0, 0x20cf},
    {"Combining Diacritical Marks for Symbols", 0x20d0, 0x20ff},
    {"Letterlike Symbols", 0x2100, 0x214f},
    {"Number Forms", 0x2150, 0x218f},
    {"Arrows", 0x2190, 0x21ff},
    {"Mathematical Operators", 0x2200, 0x22ff},
    {"Miscellaneous Technical", 0x2300, 0x23ff},
    {"Control Pictures", 0x2400, 0x243f},
    {"Optical Character Recognition", 0x2440, 0x245f},
    {"Enclosed Alphanumerics", 0x2460, 0x24ff},
    {"Box Drawing", 0x2500, 0x257f},
    {"Block Elements", 0x2580, 0x259f},
    {"Geometric Shapes", 0x25a0, 0x25ff},
    {"Miscellaneous Symbols", 0x2600, 0x26ff},
    {"Dingbats", 0x2700, 0x27bf},
    {"Miscellaneous Mathematical Symbols-A", 0x27c0, 0x27ef},
    {"Supplemental Arrows-A", 0x27f0, 0x27ff},
    {"Braille Patterns", 0x2800, 0x28ff},
    {"Supplemental Arrows-B", 0x2900, 0x297f},
    {"Miscellaneous Mathematical Symbols-B", 0x2980, 0x29ff},
    {"Supplemental Mathematical Operators", 0x2a00, 0x2aff},
    {"Miscellaneous Symbols and Arrows", 0x2b00, 0x2bff},
    {"Glagolitic", 0x2c00, 0x2c5f},
    {"Latin Extended-C", 0x2c60, 0x2c7f},
    {"Coptic", 0x2c80, 0x2cff},
    {"Georgian Supplement", 0x2d00, 0x2d2f},
    {"Tifinagh", 0x2d30, 0x2d7f},
    {"Ethiopic Extended", 0x2d80, 0x2ddf},
    {"Cyrillic Extended-A", 0x2de0, 0x2dff},
    {"Supplemental Punctuation", 0x2e00, 0x2e7f},
    {"CJK Radicals Supplement", 0x2e80, 0x2eff},
    {"Kangxi Radicals", 0x2f00, 0x2fdf},
    {"unknown", 0x2fe0, 0x2fef},
    {"Ideographic Description Characters", 0x2ff0, 0x2fff},
    {"CJK Symbols and Punctuation", 0x3000, 0x303f},
    {"Hiragana", 0x3040, 0x309f},
    {"Katakana", 0x30a0, 0x30ff},
    {"Bopomofo", 0x3100, 0x312f},
    {"Hangul Compatibility Jamo", 0x3130, 0x318f},
    {"Kanbun", 0x3190, 0x319f},
    {"Bopomofo Extended", 0x31a0, 0x31bf},
    {"CJK Strokes", 0x31c0, 0x31ef},
    {"Katakana Phonetic Extensions", 0x31f0, 0x31ff},
    {"Enclosed CJK Letters and Months", 0x3200, 0x32ff},
    {"CJK Compatibility", 0x3300, 0x33ff},
    {"CJK Unified Ideographs Extension A", 0x3400, 0x4dbf},
    {"Yijing Hexagram Symbols", 0x4dc0, 0x4dff},
    {"CJK Unified Ideographs", 0x4e00, 0x9fff},
    {"Yi Syllables", 0xa000, 0xa48f},
    {"Yi Radicals", 0xa490, 0xa4cf},
    {"Lisu", 0xa4d0, 0xa4ff},
    {"Vai", 0xa500, 0xa63f},
    {"Cyrillic Extended-B", 0xa640, 0xa69f},
    {"Bamum", 0xa6a0, 0xa6ff},
    {"Modified Tone Letters", 0xa700, 0xa71f},
    {"Latin Extended-D", 0xa720, 0xa7ff},
    {"Syloti-Nagri", 0xa800, 0xa82f},
    {"Common Indic Number Forms", 0xa830, 0xa83f},
    {"Phags-pa", 0xa840, 0xa87f},
    {"Saurashtra", 0xa880, 0xa8df},
    {"Davanagari Extended", 0xa8e0, 0xa8ff},
    {"Kayah Li", 0xa900, 0xa92f},
    {"Rejang", 0xa930, 0xa95f},
    {"Hangul Jamo Extended-A", 0xa960, 0xa97f},
    {"Javanese", 0xa980, 0xa9df},
    {"Myanmar Extended-B", 0xa9e0, 0xa9ff},
    {"Cham", 0xaa00, 0xaa5f},
    {"Myanmar Extended-A", 0xaa60, 0xaa7f},
    {"Tai Viet", 0xaa80, 0xaadf},
    {"Meetei Mayek Extensions", 0xaae0, 0xaaff},
    {"Ethiopic Extended-A", 0xab00, 0xab2f},
    {"Latin Extended-E", 0xab30, 0xab6f},
    {"-- unassigned --", 0xab70, 0xabbf},
    {"Meetei Mayek", 0xabc0, 0xabff},
    {"Hangul Syllables", 0xac00, 0xd7af},
    {"Hangul Jamo Extended-B", 0xd7b0, 0xd7ff},
    {"High Surrogates", 0xd800, 0xdb7f},
    {"High Private Use Surrogates", 0xdb80, 0xdbff},
    {"Low Surrogates", 0xdc00, 0xdfff},
    {"Private Use Area", 0xe000, 0xf8ff},
    {"CJK Compatibility Ideographs", 0xf900, 0xfaff},
    {"Alphabetic Presentation Forms", 0xfb00, 0xfb4f},
    {"Arabic Presentation Forms-A", 0xfb50, 0xfdff},
    {"Variation Selectors", 0xfe00, 0xfe0f},
    {"Vertical Forms", 0xfe10, 0xfe1f},
    {"Combining Half Marks", 0xfe20, 0xfe2f},
    {"CJK Compatibility Forms", 0xfe30, 0xfe4f},
    {"Small Form Variants", 0xfe50, 0xfe6f},
    {"Arabic Presentation Forms-B", 0xfe70, 0xfeff},
    {"Halfwidth and Fullwidth Forms", 0xff00, 0xffef},
    {"Specials", 0xfff0, 0xffff},
    {"-- four-byte unicode --", 0x10000, 0x10ffff},
};

/* Histogram of codesets. */
static long hist[sizeof(codeset) / sizeof(struct range_s)];

#define SETS (sizeof(codeset) / sizeof(struct range_s))

/* Initialize histogram. */
static void hist_init()
{
    unsigned n;

    for (n = 0; n < SETS; n++)
        hist[n] = 0;
}

/* Update histogram. */
static void hist_add(long code)
{
    unsigned n;

    for (n = 0; n < SETS; n++)
        if (code <= codeset[n].high)
            break;
    assert(n != SETS);
    hist[n]++;
}

/* Show histogram. */
static void hist_show(void)
{
    unsigned n;

    for (n = 0; n < SETS; n++)
        if (hist[n])
            printf("%s: %ld\n", codeset[n].name, hist[n]);
}

/* Show one character in a word, honoring UTF-8 when a legal UTF-8 character is
   presented, and converting control and other non-printable characters to
   escape codes.  The number of bytes consumed in str is returned.  No more
   than len will be consumed. */
static size_t show_char(unsigned char *str, size_t len)
{
    int used = 1;
    unsigned char *tmp;

    if (len < 1)
        return 0;
    used = 1;
    if (*str < 0x80)
        hist_add(*str);
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
    else {
        used = utf8bytes(str, len);
        if (used < 2) {
            printf("\\x%02x", *str);
            used = 1;
            if (*str >= 0x80)
                hist_add(-1);
        }
        else {
            fwrite(str, 1, used, stdout);
            tmp = str;
            hist_add(utf8code(&tmp, len));
        }
    }
    return (size_t)used;
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

    hist_init();
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
    puts("");
    hist_show();
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
