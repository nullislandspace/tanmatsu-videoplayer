/*
 * SIMD-optimized memcpy for ESP32-P4 PIE.
 *
 * Uses 8-register unrolled loads/stores to copy 128 bytes per loop iteration.
 * Requires 16-byte aligned src/dst and size that is a multiple of 128.
 * Falls back to standard memcpy when CONFIG_USE_SIMD_OPTIMIZATIONS is not set.
 *
 * Typical speedup: 4.3-4.5x over libc memcpy on ESP32-P4.
 */

#pragma once

#include <string.h>
#include <stdint.h>
#include <stddef.h>

#ifdef CONFIG_USE_SIMD_OPTIMIZATIONS

/*
 * Copy 'size' bytes from 'src' to 'dst' using SIMD.
 * Requirements:
 *   - src and dst must be 16-byte aligned
 *   - size must be a multiple of 128
 * For sizes not meeting these requirements, use standard memcpy.
 */
static inline void simd_memcpy_aligned(void *dst, const void *src, size_t size)
{
    register const uint8_t *s asm("a0") = (const uint8_t *)src;
    register uint8_t *d asm("a1") = (uint8_t *)dst;
    register size_t remaining asm("a2") = size;

    asm volatile(
        "1:\n"
        /* Load 8 × 16 bytes = 128 bytes */
        "esp.vld.128.ip q0, %[s], 16\n"
        "esp.vld.128.ip q1, %[s], 16\n"
        "esp.vld.128.ip q2, %[s], 16\n"
        "esp.vld.128.ip q3, %[s], 16\n"
        "esp.vld.128.ip q4, %[s], 16\n"
        "esp.vld.128.ip q5, %[s], 16\n"
        "esp.vld.128.ip q6, %[s], 16\n"
        "esp.vld.128.ip q7, %[s], 16\n"
        /* Store 8 × 16 bytes = 128 bytes */
        "esp.vst.128.ip q0, %[d], 16\n"
        "esp.vst.128.ip q1, %[d], 16\n"
        "esp.vst.128.ip q2, %[d], 16\n"
        "esp.vst.128.ip q3, %[d], 16\n"
        "esp.vst.128.ip q4, %[d], 16\n"
        "esp.vst.128.ip q5, %[d], 16\n"
        "esp.vst.128.ip q6, %[d], 16\n"
        "esp.vst.128.ip q7, %[d], 16\n"
        "addi %[rem], %[rem], -128\n"
        "bnez %[rem], 1b\n"
        : [s] "+r"(s), [d] "+r"(d), [rem] "+r"(remaining)
        :: "memory"
    );
}

/*
 * General-purpose SIMD memcpy: copies the bulk with SIMD (128-byte chunks),
 * then handles any remainder with standard memcpy.
 * src and dst must still be 16-byte aligned.
 */
static inline void simd_memcpy(void *dst, const void *src, size_t size)
{
    size_t bulk = size & ~(size_t)127;  /* round down to multiple of 128 */
    if (bulk > 0) {
        simd_memcpy_aligned(dst, src, bulk);
    }
    size_t tail = size - bulk;
    if (tail > 0) {
        memcpy((uint8_t *)dst + bulk, (const uint8_t *)src + bulk, tail);
    }
}

#else /* !CONFIG_USE_SIMD_OPTIMIZATIONS */

static inline void simd_memcpy_aligned(void *dst, const void *src, size_t size)
{
    memcpy(dst, src, size);
}

static inline void simd_memcpy(void *dst, const void *src, size_t size)
{
    memcpy(dst, src, size);
}

#endif /* CONFIG_USE_SIMD_OPTIMIZATIONS */
