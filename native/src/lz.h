/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_LZ_H
#define SD_LZ_H

#include <stddef.h>
#include <stdint.h>

static inline size_t sd_lz_bound(size_t n) { return n + n / 255 + 32; }

size_t sd_lz_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t dst_cap);

int sd_lz_decompress(const uint8_t *src, size_t n, uint8_t *dst, size_t raw_size);

#endif
