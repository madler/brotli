/*
 * yeast.h
 * Copyright (C) 2015 Mark Adler
 * For conditions of distribution and use, see the accompanying LICENSE file.
 */

/*
 * Decompress the compressed brolti stream in source[0..*len-1].  On return,
 * *len is updated to the number of bytes used in the compressed stream, *dest
 * is an allocated buffer with the uncompressed data, and *got is the number of
 * bytes of uncompressed data.  If *got is zero, then *dest is NULL.  yeast()
 * returns 0 on success, or non-zero on failure.  The return values are 0 for
 * success, 1 for out of memory, 2 for a premature end of input, or 3 for
 * invalid compressed data.  On failure, *len, *dest, and *got are set as
 * described, reflecting the state at the point of failure.
 */
int yeast(void const *source, size_t *len, void **dest, size_t *got);
