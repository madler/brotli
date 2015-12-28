#include "huff.h"

// See description in huff.h.  This algorithm is due to Alistair Moffat and
// Jyrki Katajainen.
void huffman(len_t *bits, freq_t *freq, size_t len) {
    // deal with trivial cases
    if (len <= 0)
        return;
    if (len == 1) {
        bits[0] = 0;
        return;
    }

    // first pass, left to right, setting parent pointers
    freq[0] += freq[1];
    len_t root = 0;             // next root node to be used
    len_t leaf = 2;             // next leaf to be used
    len_t next;                 // next value to be assigned
    for (next = 1; next < len - 1; next++) {
        // select first item for a pairing
        if (leaf >= len || freq[root] < freq[leaf]) {
            freq[next] = freq[root];
            bits[root++] = next;
        }
        else
            freq[next] = freq[leaf++];

        // add on the second item
        if (leaf >= len || (root < next && freq[root] < freq[leaf])) {
            freq[next] += freq[root];
            bits[root++] = next;
        }
        else
            freq[next] += freq[leaf++];
    }

    // second pass, right to left, setting internal depths
    next = len - 2;
    bits[next] = 0;
    while (next--)
        bits[next] = bits[bits[next]] + 1;

    // third pass, right to left, setting leaf depths
    len_t avbl = 1;             // number of available nodes
    len_t used = 0;             // number of internal nodes
    len_t dpth = 0;             // current depth of leaves
    root = next = len - 1;
    while (avbl) {
        while (root && (bits[root - 1] == dpth)) {
            used++;
            root--;
        }
        while (avbl > used) {
            bits[next--] = dpth;
            avbl--;
        }
        avbl = used << 1;
        dpth++;
        used = 0;
    }
}
