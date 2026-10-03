/* lz.h - a small, fast LZ77 block compressor (LZ4-style format), used for scrollback.
 * No dependencies. The decoder validates every length and offset, so corrupt input fails
 * cleanly instead of reading or writing out of bounds. */
#ifndef SD_LZ_H
#define SD_LZ_H

#include <stddef.h>
#include <stdint.h>

/* Worst-case compressed size for n input bytes */
static inline size_t sd_lz_bound(size_t n) { return n + n / 255 + 32; }

/* Returns the compressed size, or 0 if dst_cap is too small. */
size_t sd_lz_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t dst_cap);

/* Decompresses exactly raw_size bytes into dst. Returns 1 on success, 0 on malformed input. */
int sd_lz_decompress(const uint8_t *src, size_t n, uint8_t *dst, size_t raw_size);

#endif
