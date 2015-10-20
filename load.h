/* Load the entire input from in into a memory buffer.  The allocated memory
   buffer is returned in *dat, which points to the loaded data, with the number
   of bytes of data at *dat returned in *len.

   If limit is not zero, then *len is constrained to be less than or equal to
   limit.  If limit is zero, then *len is only limited to the maximum size_t
   value.  The file pointer is left at the end of the file, or pointing after
   the *len or maximum size_t bytes that were read.

   If *dat on entry is not NULL, then it is assumed to be an allocated buffer
   of size *size.  If *dat on entry is NULL, then a new buffer is allocated and
   the entry value of *size is ignored and overwritten.  A supplied buffer is
   never reduced in size, but may be increased in size which may result in a
   new address, returned in *dat.  In any case, *size returns the final size of
   the allocation at *dat.  This enables reuse of an existing allocation over
   multiple load() calls.

   load() returns 0 on success, 1 if limit or the maximum size_t value was
   reached with more data remaining in the input, -1 if there is a read error,
   or -2 if out of memory.  In all cases, *dat, *size, and *len return with
   their last values, so *dat contains *len bytes from in, and *dat is an
   allocation of size *size.
 */

#include <stdio.h>
#include <stddef.h>

int load(FILE *in, size_t limit, void **dat, size_t *size, size_t *len);
