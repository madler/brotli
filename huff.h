#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

// Types for huffman() -- len_t should be able to represent the largest number
// of symbols to code.  len_t must be an integer type, signed or unsigned.
// freq_t can be any numeric type that can be added and compared.  freq_t must
// be able to represent the sum of all of the frequencies.
typedef unsigned short freq_t;
typedef unsigned short len_t;

// Apply Huffman's algorithm to the set of frequencies freq[0..len-1], which
// must be positive and in non-decreasing order, returning the optimal number
// of bits for each symbol in the respective locations in bits[0..len-1].  If
// sizeof(len_t) <= sizeof(freq_t), then freq and bits can point to the same
// memory for in-place generation of the Huffman code lengths.  Whether or not
// bits points to freq, freq[] will be modified.
void huffman(len_t *bits, freq_t *freq, size_t len);
#ifdef __cplusplus
}
#endif
