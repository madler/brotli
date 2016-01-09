// .br format constants

// Signature
#define BR_SIG "\xce\xb2\xcf\x81"

// Content mask (high bit is parity)
#define BR_CONTENT_CHECK 7          // check value type (3 bits)
#define BR_CONTENT_LEN 8            // uncompressed length present
#define BR_CONTENT_OFF 0x10         // offset to previous header present
#define BR_CONTENT_TRAIL 0x20       // this is a trailer content mask
#define BR_CONTENT_EXTRA_MASK 0x40  // an Extra mask follows this or Check ID

// Check values in Content mask
#define BR_CHECK_XXH32_1 0          // low byte of XXH32
#define BR_CHECK_XXH32_2 1          // low two bytes of XXH32
#define BR_CHECK_XXH32_4 2          // XXH32
#define BR_CHECK_XXH64_8 3          // XXH64
#define BR_CHECK_CRC32_1 4          // low byte of CRC-32C
#define BR_CHECK_CRC32_2 5          // low two bytes of CRC-32C
#define BR_CHECK_CRC32_4 6          // CRC-32C
#define BR_CHECK_ID 7               // use the Check ID that follows

// Check ID
#define BR_CHECKID_SHA256 0         // use SHA256 (32 bytes)
#define BR_CHECKID_UNKNOWN 1        // id's >= this are unknown

// Extra mask (high bit is parity)
#define BR_EXTRA_MOD 1              // modification time present
#define BR_EXTRA_NAME 2             // file name present
#define BR_EXTRA_EXTRA 4            // extra field present
#define BR_EXTRA_RESERVED 0x18      // must be zeros
#define BR_EXTRA_CHECK 0x20         // header check is present
#define BR_EXTRA_COMPRESSION_MASK 0x40  // Compression mask follows

// Compression Mask (high bit is parity)
#define BR_COMPRESSION_METHOD 7     // compression method -- must be 0 (brotli)
#define BR_COMPRESSION_CONSTRAINTS 0x38 // compression constraints (ignorable)
#define BR_COMPRESSION_RESERVED 0x40    // must be zero
