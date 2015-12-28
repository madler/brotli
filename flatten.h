#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

// Type for code lengths vector.  This can be an unsigned integer of any size.
typedef unsigned short bits_t;

// Flatten the code lengths in bits[0..n-1] to lengths no more than limit using
// a greedy approach.  bits[] must be in non-increasing order.  limit must be
// large enough to encode n symbols, i.e. 1 << limit must be greater than or
// equal to n.  flatten() returns true on failure.
int flatten(bits_t *bits, size_t n, unsigned limit);
#ifdef __cplusplus
}
#endif
