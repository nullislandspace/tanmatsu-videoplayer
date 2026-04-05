/*
 * SIMD (PIE) implementation of FilterHorLuma for ESP32-P4.
 *
 * Hybrid SIMD threshold + scalar filter.
 *
 *   Phase 1: Single asm block loads 4 rows via usar, computes 3 threshold
 *     tests vectorially, stores 16-byte mask. Pre-computed row pointers
 *     and vldbc.8 broadcasts eliminate memcpy/memset overhead.
 *   Phase 2: Scalar filter iterates mask_bytes[] directly (no bitmask
 *     conversion). Early-out via u32 OR check when no pixels need filtering.
 *
 * Measured: 3.04x speedup vs. scalar C reference.
 *
 * This entire file is guarded by CONFIG_USE_SIMD_OPTIMIZATIONS so that the
 * project can be compiled for other processors without this implementation.
 */

#include "sdkconfig.h"

#ifdef CONFIG_USE_SIMD_OPTIMIZATIONS

#include "basetype.h"
#include "h264bsd_util.h"

/* edgeThreshold_t is file-local to h264bsd_deblocking.c. We redeclare it
 * here with identical layout. C linkage does not encode struct types, so
 * this is compatible with the caller across translation units. */
typedef struct {
    const u8 *tc0;
    u32 alpha;
    u32 beta;
} edgeThreshold_t;

/* Clipping table defined in intra_prediction.c */
extern const u8 h264bsdClip[];

void FilterHorLuma(
  u8 *data,
  u32 bS,
  edgeThreshold_t *thresholds,
  i32 imageWidth)
{
    u32 alpha = thresholds->alpha;
    u32 beta = thresholds->beta;

    /* Store threshold bytes for vldbc broadcast (single byte each, aligned) */
    u8 __attribute__((aligned(16))) thresh_bytes[16];
    thresh_bytes[0] = (u8)alpha;
    thresh_bytes[8] = (u8)beta;  /* offset 8 so second vldbc.8.ip can use stride */

    /* Pre-compute row pointers */
    u8 *row_p1_ptr = data - imageWidth * 2;
    u8 *row_p0_ptr = data - imageWidth;
    u8 *row_q0_ptr = data;
    u8 *row_q1_ptr = data + imageWidth;

    u8 __attribute__((aligned(16))) mask_bytes[16];

    /*
     * Phase 1: Single asm block — usar load 4 rows, broadcast thresholds
     * via vldbc.8, compute threshold mask, store result.
     *
     * q0=p1, q1=p0, q2=q0, q3=q1, q4=alpha, q5=beta, q6=mask, q7=temp
     */
    {
        register u8 *r0 asm("a0") = row_p1_ptr;
        register u8 *r1 asm("a1") = row_p0_ptr;
        register u8 *r2 asm("a2") = row_q0_ptr;
        register u8 *r3 asm("a3") = row_q1_ptr;
        register u8 *r4 asm("a4") = thresh_bytes;       /* alpha at [0] */
        register u8 *r5 asm("a5") = thresh_bytes + 8;   /* beta at [8] */

        asm volatile(
            /* Load 4 rows via usar (unaligned) */
            /* p1 -> q0 */
            "esp.ld.128.usar.ip q0, %[r0], 16\n"
            "esp.ld.128.usar.ip q7, %[r0], 0\n"
            "esp.src.q q0, q0, q7\n"
            /* p0 -> q1 */
            "esp.ld.128.usar.ip q1, %[r1], 16\n"
            "esp.ld.128.usar.ip q7, %[r1], 0\n"
            "esp.src.q q1, q1, q7\n"
            /* q0_row -> q2 */
            "esp.ld.128.usar.ip q2, %[r2], 16\n"
            "esp.ld.128.usar.ip q7, %[r2], 0\n"
            "esp.src.q q2, q2, q7\n"
            /* q1_row -> q3 */
            "esp.ld.128.usar.ip q3, %[r3], 16\n"
            "esp.ld.128.usar.ip q7, %[r3], 0\n"
            "esp.src.q q3, q3, q7\n"

            /* Broadcast alpha and beta via vldbc.8 (1 byte -> 16 lanes) */
            "esp.vldbc.8.ip q4, %[r4], 0\n"
            "esp.vldbc.8.ip q5, %[r5], 0\n"

            /* Compute |p0 - q0| < alpha */
            "esp.vsub.u8 q6, q1, q2\n"
            "esp.vsub.u8 q7, q2, q1\n"
            "esp.vmax.u8 q6, q6, q7\n"
            "esp.vcmp.lt.u8 q6, q6, q4\n"

            /* Compute |p1 - p0| < beta, AND */
            "esp.vsub.u8 q7, q0, q1\n"
            "esp.vsub.u8 q4, q1, q0\n"
            "esp.vmax.u8 q7, q7, q4\n"
            "esp.vcmp.lt.u8 q7, q7, q5\n"
            "esp.andq q6, q6, q7\n"

            /* Compute |q1 - q0| < beta, AND */
            "esp.vsub.u8 q7, q3, q2\n"
            "esp.vsub.u8 q4, q2, q3\n"
            "esp.vmax.u8 q7, q7, q4\n"
            "esp.vcmp.lt.u8 q7, q7, q5\n"
            "esp.andq q6, q6, q7\n"

            : [r0] "+r"(r0), [r1] "+r"(r1), [r2] "+r"(r2),
              [r3] "+r"(r3), [r4] "+r"(r4), [r5] "+r"(r5)
            :
            : "memory"
        );

        /* Store mask using r0 (freed after usar loads) */
        r0 = mask_bytes;
        asm volatile(
            "esp.vst.128.ip q6, %[r0], 0\n"
            : [r0] "+r"(r0) :: "memory"
        );
    }

    /* Early-out: OR all 4 mask words, skip if no pixels need filtering */
    {
        const u32 *mw = (const u32 *)mask_bytes;
        if ((mw[0] | mw[1] | mw[2] | mw[3]) == 0) return;
    }

    /*
     * Phase 2: scalar filter for active pixels.
     * Check mask_bytes[i] directly — avoids bitmask conversion overhead.
     */
    if (bS < 4)
    {
        const u8 *clp = h264bsdClip + 512;
        i32 tc = thresholds->tc0[bS - 1];

        u8 *dp = data;
        for (u32 i = 0; i < 16; i++, dp++)
        {
            if (!mask_bytes[i]) continue;

            i32 p1 = dp[-imageWidth * 2];
            i32 p0 = dp[-imageWidth];
            i32 q0 = dp[0];
            i32 q1 = dp[imageWidth];
            i32 tmp = tc;
            i32 val;

            i32 p2 = dp[-imageWidth * 3];
            if ((u32)ABS(p2 - p0) < beta)
            {
                val = (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1;
                dp[-imageWidth * 2] = (u8)(p1 + CLIP3(-tc, tc, val));
                tmp++;
            }

            i32 q2 = dp[imageWidth * 2];
            if ((u32)ABS(q2 - q0) < beta)
            {
                val = (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1;
                dp[imageWidth] = (u8)(q1 + CLIP3(-tc, tc, val));
                tmp++;
            }

            val = ((((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
            i32 delta = CLIP3(-tmp, tmp, val);

            dp[-imageWidth] = clp[p0 + delta];
            dp[0] = clp[q0 - delta];
        }
    }
    else /* bS == 4 */
    {
        u8 *dp = data;
        for (u32 i = 0; i < 16; i++, dp++)
        {
            if (!mask_bytes[i]) continue;

            i32 p1 = dp[-imageWidth * 2];
            i32 p0 = dp[-imageWidth];
            i32 q0 = dp[0];
            i32 q1 = dp[imageWidth];
            i32 tmp;

            u32 tmpFlag = ((u32)ABS(p0 - q0) < ((alpha >> 2) + 2))
                            ? HANTRO_TRUE : HANTRO_FALSE;

            i32 p2 = dp[-imageWidth * 3];
            i32 q2 = dp[imageWidth * 2];

            if (tmpFlag && (u32)ABS(p2 - p0) < beta)
            {
                tmp = p1 + p0 + q0;
                dp[-imageWidth] = (u8)((p2 + 2 * tmp + q1 + 4) >> 3);
                dp[-imageWidth * 2] = (u8)((p2 + tmp + 2) >> 2);
                dp[-imageWidth * 3] = (u8)((2 * dp[-imageWidth * 4] +
                                       3 * p2 + tmp + 4) >> 3);
            }
            else
                dp[-imageWidth] = (u8)((2 * p1 + p0 + q1 + 2) >> 2);

            if (tmpFlag && (u32)ABS(q2 - q0) < beta)
            {
                tmp = p0 + q0 + q1;
                dp[0] = (u8)((p1 + 2 * tmp + q2 + 4) >> 3);
                dp[imageWidth] = (u8)((tmp + q2 + 2) >> 2);
                dp[imageWidth * 2] = (u8)((2 * dp[imageWidth * 3] +
                                      3 * q2 + tmp + 4) >> 3);
            }
            else
                dp[0] = (2 * q1 + q0 + p1 + 2) >> 2;
        }
    }
}

#endif /* CONFIG_USE_SIMD_OPTIMIZATIONS */
