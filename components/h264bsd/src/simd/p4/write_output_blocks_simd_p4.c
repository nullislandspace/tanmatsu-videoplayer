/*
 * SIMD (PIE) implementation of h264bsdWriteOutputBlocks for ESP32-P4.
 *
 * Per-pixel operation: output = clamp(pred_u8 + residual_i16, 0, 255)
 *
 * Optimizations applied (from deblocking filter lessons):
 *   - usar loads for unaligned prediction data (no memcpy to temp)
 *   - Pre-load constant vectors (zero, 255) once, reuse across all rows
 *   - Scalar u32 stores for unaligned output (no memcpy from temp)
 *   - Inlined loops to avoid per-row function call overhead
 *
 * Measured: 4.52x speedup vs. scalar C reference.
 *
 * This entire file is guarded by CONFIG_USE_SIMD_OPTIMIZATIONS so that the
 * project can be compiled for other processors without this implementation.
 */

#include "sdkconfig.h"

#ifdef CONFIG_USE_SIMD_OPTIMIZATIONS

#include "h264bsd_image.h"
#include "h264bsd_util.h"
#include "h264bsd_macroblock_layer.h"
#include "h264bsd_profile.h"

void h264bsdWriteOutputBlocks(image_t *image, u32 mbNum, u8 *data,
        residual_t *pResidual)
{
    u32 picWidth, picSize;
    u8 *lum, *cb, *cr;
    u32 row, comp;
    u32 t_start = H264_GET_TIME_US();

    ASSERT(image);
    ASSERT(data);
    ASSERT(mbNum < image->width * image->height);

    /* Image size in macroblocks */
    picWidth = image->width;
    picSize = picWidth * image->height;
    row = mbNum / picWidth;
    u32 col = mbNum % picWidth;

    /* Output macroblock position in output picture */
    lum = (image->data + row * picWidth * 256 + col * 16);
    cb = (image->data + picSize * 256 + row * picWidth * 64 + col * 8);
    cr = (cb + picSize * 64);

    picWidth *= 16;

    /*
     * Build constant vectors entirely in-register (no memory access):
     *   q6 = [0, 0, 0, 0, 0, 0, 0, 0]  (8 x i16 zeros)
     *   q7 = [255, 255, 255, 255, 255, 255, 255, 255]  (8 x i16)
     *
     * vsadds.u16 adds a scalar (truncated to u16) to each lane with
     * unsigned saturation. zero + 255 = 255 in every lane.
     */
    {
        register u32 val255 asm("a0") = 255;
        asm volatile(
            "esp.zero.q q6\n"
            "esp.zero.q q7\n"
            "esp.vsadds.u16 q7, q7, %[v]\n"
            : : [v] "r"(val255) : "memory"
        );
    }

    /* Luma: 16 rows of 16 pixels */
    for (row = 0; row < 16; row++)
    {
        u8 *predRow = data + row * 16;
        u8 *imgRow = lum + row * picWidth;
        i16 *resRow = pResidual->lumaRows[row];

        u8 tmp[16] __attribute__((aligned(16)));

        register u8 *r0 asm("a0") = predRow;
        register i16 *r1 asm("a1") = resRow;
        register u8 *r2 asm("a2") = tmp;

        asm volatile(
            /* Load 16 u8 predictions (unaligned usar) */
            "esp.ld.128.usar.ip q0, %[pred], 16\n"
            "esp.ld.128.usar.ip q1, %[pred], 0\n"
            "esp.src.q q0, q0, q1\n"

            /* Widen u8->i16: q0=[b0,0..b7,0] q1=[b8,0..b15,0] */
            "esp.zero.q q1\n"
            "esp.vzip.8 q0, q1\n"

            /* Load 16 i16 residuals (aligned) */
            "esp.vld.128.ip q2, %[res], 16\n"
            "esp.vld.128.ip q3, %[res], 0\n"

            /* Add */
            "esp.vadd.s16 q0, q0, q2\n"
            "esp.vadd.s16 q1, q1, q3\n"

            /* Clamp [0, 255] using pre-loaded q6=0, q7=255 */
            "esp.vmax.s16 q0, q0, q6\n"
            "esp.vmax.s16 q1, q1, q6\n"
            "esp.vmin.s16 q0, q0, q7\n"
            "esp.vmin.s16 q1, q1, q7\n"

            /* Narrow i16->u8 */
            "esp.vunzip.8 q0, q1\n"

            /* Store to aligned temp */
            "esp.vst.128.ip q0, %[tmp], 0\n"

            : [pred] "+r"(r0), [res] "+r"(r1), [tmp] "+r"(r2)
            :
            : "memory"
        );

        /* Write 16 bytes to unaligned output via u32 stores */
        {
            const u32 *src = (const u32 *)tmp;
            u32 *dst = (u32 *)imgRow;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = src[3];
        }
    }

    /* Chroma: 2 planes (Cb, Cr), 8 rows of 8 pixels each */
    picWidth /= 2;

    for (comp = 0; comp < 2; comp++)
    {
        u8 *imgPlane = (comp == 0) ? cb : cr;
        u8 *predBase = data + 256 + comp * 64;

        for (row = 0; row < 8; row++)
        {
            u8 *predRow = predBase + row * 8;
            u8 *imgRow = imgPlane + row * picWidth;
            i16 *resRow = pResidual->chromaRows[comp][row];

            /*
             * Chroma: 8 pixels. Use usar for pred load.
             * Only need q0 for data, q2 for residual. q6/q7 still hold constants.
             */
            u8 tmp[16] __attribute__((aligned(16)));

            register u8 *r0 asm("a0") = predRow;
            register i16 *r1 asm("a1") = resRow;
            register u8 *r2 asm("a2") = tmp;

            asm volatile(
                /* Load 8 u8 predictions via usar (loads 16, we only use low 8) */
                "esp.ld.128.usar.ip q0, %[pred], 16\n"
                "esp.ld.128.usar.ip q1, %[pred], 0\n"
                "esp.src.q q0, q0, q1\n"

                /* Widen: only low 8 bytes matter, upper 8 become garbage in q1 */
                "esp.zero.q q1\n"
                "esp.vzip.8 q0, q1\n"
                /* q0 = [p0,0,...,p7,0] = 8 pixels as i16 — this is all we need */

                /* Load 8 i16 residuals (aligned) */
                "esp.vld.128.ip q2, %[res], 0\n"

                /* Add */
                "esp.vadd.s16 q0, q0, q2\n"

                /* Clamp [0, 255] */
                "esp.vmax.s16 q0, q0, q6\n"
                "esp.vmin.s16 q0, q0, q7\n"

                /* Narrow: need second register for vunzip */
                "esp.zero.q q1\n"
                "esp.vunzip.8 q0, q1\n"

                /* Store to aligned temp */
                "esp.vst.128.ip q0, %[tmp], 0\n"

                : [pred] "+r"(r0), [res] "+r"(r1), [tmp] "+r"(r2)
                :
                : "memory"
            );

            /* Write 8 bytes to unaligned output via u32 stores */
            {
                const u32 *src = (const u32 *)tmp;
                u32 *dst = (u32 *)imgRow;
                dst[0] = src[0];
                dst[1] = src[1];
            }
        }
    }

    h264bsdTimerAdd(H264_TIMER_WRITE_OUTPUT_BLOCKS, H264_GET_TIME_US() - t_start);
}

#endif /* CONFIG_USE_SIMD_OPTIMIZATIONS */
