/*
 * yeast.h
 * Copyright (C) 2015 Mark Adler
 * For conditions of distribution and use, see the accompanying LICENSE file.
 */

/*
 * Decompress the compressed brolti stream in source[0..*len-1].  If cmp is
 * false, return the decompressed data.  In this case, on return *len is
 * updated to the number of bytes used in the compressed stream, *dest is an
 * allocated buffer with the uncompressed data, and *got is the number of bytes
 * of uncompressed data.  If *got is zero, then *dest is NULL.  yeast() returns
 * 0 on success, or non-zero on failure.  The return values are 0 for success,
 * 1 for out of memory, 2 for a premature end of input, or 3 for invalid
 * compressed data.  On failure, *len, *dest, and *got are set as described,
 * reflecting the state at the point of failure.
 *
 * If cmp is true, then a compare is done instead.  In this case, *dest and
 * *got must have the expected uncompressed data and length.  If *got is zero,
 * then *dest is ignored.  In addition to the return values above, a return
 * value of 4 indicates a compare mismatch.  On return, *got is updated to the
 * end of the last literal or string copied before the mismatch was detected.
 * *dest is unchanged.
 */
int yeast(void **dest, size_t *got, void const *source, size_t *len, int cmp);
