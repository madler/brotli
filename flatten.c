#include "flatten.h"

// Flatten a generated Huffman code to a maximum bit length, using a greedy
// approach that only uses the sorted sequence of bit lengths.
int flatten(bits_t *bits, size_t n, unsigned limit)
{
    // Handle trivial and invalid cases.
    if (n == 0)
        return 0;
    const unsigned max = bits[0];
    if (max <= limit)
        return 0;
    if (((size_t)1 << limit) < n)
        return 1;

    // Push codes longer than limit to limit, accumulating debt.
    size_t k = 0;
    long debt = 0;
    const long to = 1L << (max - limit);
    do {
        debt += to - (1L << (max - bits[k]));
        bits[k++] = limit;
    } while (k < n && bits[k] > limit);
    debt >>= max - limit;       // scale debt to limit (bits shifted are zero)

    // Scan to the first code whose length is less than limit.
    while (k < n && bits[k] == limit)
        k++;

    // Increase the lengths of codes less than limit up to limit until the debt
    // is fully paid, which may result in the debt being overpaid.
    while (k < n) {
        long pay = (1L << (limit - bits[k])) - 1;
        bits[k++] = limit;
        debt -= pay;
        if (debt <= 0)
            break;
    }

    // If the debt is paid off, then done.
    if (debt == 0)
        return 0;

    // Decrease the length of the codes by one from the bottom up to reduce the
    // overpaid debt to zero, but don't overspend the negative debt.
    k = n;
    while (k && bits[k - 1] == 1)
        k--;                        // can't decrease length-one codes
    while (k) {
        long spend = 1L << (limit - bits[k - 1]);
        if (debt + spend > 0) {
            k--;
            continue;
        }
        bits[--k]--;
        debt += spend;
        if (debt == 0)
            break;
    }

    // Return failure if the debt is still not exactly zero.
    return debt != 0;
}
