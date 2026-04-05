/*
 * Copyright (C) 2009 The Android Open Source Project
 * Modified for use by h264bsd standalone library
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*------------------------------------------------------------------------------

    Table of contents

     1. Include headers
     2. External compiler flags
     3. Module defines
     4. Local function prototypes
     5. Functions
          h264bsdWriteMacroblock
          h264bsdWriteOutputBlocks

------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "sdkconfig.h"
#include "h264bsd_image.h"
#include "h264bsd_util.h"
#include "h264bsd_neighbour.h"
#include "h264bsd_macroblock_layer.h"
#include <string.h>

#include "h264bsd_profile.h"

/* Timer array definition — single instance, declared extern in h264bsd_profile.h */
u32 h264bsd_timers[H264_TIMER_COUNT];
u32 h264bsd_luma_case_counts[16];

void h264bsdResetTimers(void) {
    for (int i = 0; i < H264_TIMER_COUNT; i++)
        h264bsd_timers[i] = 0;
    for (int i = 0; i < 16; i++)
        h264bsd_luma_case_counts[i] = 0;
}

u32 h264bsdGetTimer(int id) {
    if (id >= 0 && id < H264_TIMER_COUNT)
        return h264bsd_timers[id];
    return 0;
}

/*------------------------------------------------------------------------------
    2. External compiler flags
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
    3. Module defines
------------------------------------------------------------------------------*/

/* x- and y-coordinates for each block, defined in h264bsd_intra_prediction.c */
extern const u32 h264bsdBlockX[];
extern const u32 h264bsdBlockY[];

/* clipping table, defined in h264bsd_intra_prediction.c */
extern const u8 h264bsdClip[];

/*------------------------------------------------------------------------------
    4. Local function prototypes
------------------------------------------------------------------------------*/



/*------------------------------------------------------------------------------

    Function: h264bsdWriteMacroblock

        Functional description:
            Write one macroblock into the image. Both luma and chroma
            components will be written at the same time.

        Inputs:
            data    pointer to macroblock data to be written, 256 values for
                    luma followed by 64 values for both chroma components

        Outputs:
            image   pointer to the image where the macroblock will be written

        Returns:
            none

------------------------------------------------------------------------------*/
void h264bsdWriteMacroblock(image_t *image, u8 *data)
{

/* Variables */

    u32 i;
    u32 width;
    u32 *lum, *cb, *cr;
    u32 *ptr;
    u32 tmp1, tmp2;

/* Code */

    ASSERT(image);
    ASSERT(data);
    ASSERT(!((u32)data&0x3));

    width = image->width;

    /*lint -save -e826 lum, cb and cr used to copy 4 bytes at the time, disable
     * "area too small" info message */
    lum = (u32*)image->luma;
    cb = (u32*)image->cb;
    cr = (u32*)image->cr;
    ASSERT(!((u32)lum&0x3));
    ASSERT(!((u32)cb&0x3));
    ASSERT(!((u32)cr&0x3));

    ptr = (u32*)data;

    width *= 4;
    for (i = 16; i ; i--)
    {
        tmp1 = *ptr++;
        tmp2 = *ptr++;
        *lum++ = tmp1;
        *lum++ = tmp2;
        tmp1 = *ptr++;
        tmp2 = *ptr++;
        *lum++ = tmp1;
        *lum++ = tmp2;
        lum += width-4;
    }

    width >>= 1;
    for (i = 8; i ; i--)
    {
        tmp1 = *ptr++;
        tmp2 = *ptr++;
        *cb++ = tmp1;
        *cb++ = tmp2;
        cb += width-2;
    }

    for (i = 8; i ; i--)
    {
        tmp1 = *ptr++;
        tmp2 = *ptr++;
        *cr++ = tmp1;
        *cr++ = tmp2;
        cr += width-2;
    }

}
/*------------------------------------------------------------------------------

    Function: h264bsdReorderResidualToRows

        Functional description:
            Reorder i16 residuals from block-order to row-order for
            SIMD-friendly access in h264bsdWriteOutputBlocks.

        SIMD CANDIDATE: h264bsdReorderResidualToRows

        Reorders 4x4 block residuals from block-order to row-order.
        Input:  levelI16[24][16] — i16, 16-byte aligned, 24 blocks of
                16 coefficients each (4 rows x 4 cols in raster order).
        Output: lumaRows[16][16] — i16, 16-byte aligned, 16 rows of
                16 contiguous pixels covering the full 16x16 macroblock.
                chromaRows[2][8][8] — i16, 16-byte aligned, 2 planes
                (Cb, Cr) of 8 rows x 8 pixels.

        Each 4x4 block (16 i16 = 32 bytes) is scattered to 4 rows of
        4 values. Empty blocks produce zeros in the output.

------------------------------------------------------------------------------*/

void h264bsdReorderResidualToRows(residual_t *pResidual)
{
    u32 block, row, col;
    u32 x, y;

    /* Luma: blocks 0-15 → lumaRows[16][16] */
    for (block = 0; block < 16; block++)
    {
        x = h264bsdBlockX[block];
        y = h264bsdBlockY[block];

        if (IS_RESIDUAL_EMPTY_I16(pResidual->levelI16[block]))
        {
            for (row = 0; row < 4; row++)
                memset(&pResidual->lumaRows[y + row][x], 0, 4 * sizeof(i16));
        }
        else
        {
            const i16 *src = pResidual->levelI16[block];
            for (row = 0; row < 4; row++)
                for (col = 0; col < 4; col++)
                    pResidual->lumaRows[y + row][x + col] = src[row * 4 + col];
        }
    }

    /* Chroma: blocks 16-19 → chromaRows[0] (Cb), blocks 20-23 → chromaRows[1] (Cr) */
    for (block = 16; block < 24; block++)
    {
        u32 comp = (block >= 20) ? 1 : 0;
        x = h264bsdBlockX[block & 0x3];
        y = h264bsdBlockY[block & 0x3];

        if (IS_RESIDUAL_EMPTY_I16(pResidual->levelI16[block]))
        {
            for (row = 0; row < 4; row++)
                memset(&pResidual->chromaRows[comp][y + row][x], 0, 4 * sizeof(i16));
        }
        else
        {
            const i16 *src = pResidual->levelI16[block];
            for (row = 0; row < 4; row++)
                for (col = 0; col < 4; col++)
                    pResidual->chromaRows[comp][y + row][x + col] = src[row * 4 + col];
        }
    }
}

/*------------------------------------------------------------------------------

    Function: h264bsdWriteOutputBlocks

        Functional description:
            Write one macroblock into the image. Prediction and row-ordered
            i16 residuals are combined and clipped to [0,255].

        SIMD CANDIDATE: h264bsdWriteOutputBlocks

        Adds i16 residuals to u8 predictions, clips to [0,255], writes
        to image.
        Input:  data[256+128] — u8 prediction, 4-byte aligned,
                stride 16 (luma) / 8 (chroma).
                pResidual->lumaRows[16][16] — i16, 16-byte aligned,
                contiguous rows.
                pResidual->chromaRows[2][8][8] — i16, 16-byte aligned.
        Output: image luma/cb/cr planes — u8, stride = picWidth.

        Hot path: 16 pixels per row x 16 rows = 256 adds + clips (luma).
        SIMD approach: widen u8 pred to i16 (vzip.8), vadd.s16 with
        residual, vsat + narrow to u8. 16 pixels per row processed in
        two 8-pixel halves using 128-bit registers.

        Inputs:
            data        pointer to macroblock prediction data, 256 values for
                        luma followed by 64 values for both chroma components
            mbNum       number of the macroblock
            pResidual   pointer to residual_t with row-ordered i16 residuals

        Outputs:
            image       pointer to the image where the data will be written

        Returns:
            none

------------------------------------------------------------------------------*/

#ifndef CONFIG_USE_SIMD_OPTIMIZATIONS

void h264bsdWriteOutputBlocks(image_t *image, u32 mbNum, u8 *data,
        residual_t *pResidual)
{

/* Variables */

    u32 picWidth, picSize;
    u8 *lum, *cb, *cr;
    u32 row, col, comp;
    const u8 *clp = h264bsdClip + 512;
    u32 t_start = H264_GET_TIME_US();

/* Code */

    ASSERT(image);
    ASSERT(data);
    ASSERT(mbNum < image->width * image->height);

    /* Image size in macroblocks */
    picWidth = image->width;
    picSize = picWidth * image->height;
    row = mbNum / picWidth;
    col = mbNum % picWidth;

    /* Output macroblock position in output picture */
    lum = (image->data + row * picWidth * 256 + col * 16);
    cb = (image->data + picSize * 256 + row * picWidth * 64 + col * 8);
    cr = (cb + picSize * 64);

    picWidth *= 16;

    /* Luma: 16 rows of 16 pixels, reading from contiguous row-ordered i16 */
    for (row = 0; row < 16; row++)
    {
        u8 *predRow = data + row * 16;
        u8 *imgRow = lum + row * picWidth;
        i16 *resRow = pResidual->lumaRows[row];

        for (col = 0; col < 16; col++)
            imgRow[col] = clp[predRow[col] + resRow[col]];
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

            for (col = 0; col < 8; col++)
                imgRow[col] = clp[predRow[col] + resRow[col]];
        }
    }

    h264bsdTimerAdd(H264_TIMER_WRITE_OUTPUT_BLOCKS, H264_GET_TIME_US() - t_start);
}

#endif /* !CONFIG_USE_SIMD_OPTIMIZATIONS */

/*------------------------------------------------------------------------------

    Function: h264bsdConvertImageToPpa

        Functional description:
            Convert a complete I420 image to PPA-packed YUV420 format.
            Called after deblocking filter has been applied to the image.

        SIMD CANDIDATE: h264bsdConvertImageToPpa

        Converts full I420 image to PPA-packed YUV420 format.
        Input:  image->data — u8 I420 planar (Y + U + V), 16-byte aligned
                image->width/height — dimensions in macroblocks
        Output: image->ppa_data — PPA-packed buffer, 16-byte aligned
                image->ppa_stride — bytes per PPA row

        PPA format per 8 luma pixels (12 bytes):
          Even rows (0-indexed): [U0][Y0][Y1][U2] [Y2][Y3][U4][Y4] [Y5][U6][Y6][Y7]
          Odd rows (0-indexed):  [V0][Y0][Y1][V2] [Y2][Y3][V4][Y4] [Y5][V6][Y6][Y7]

        SIMD approach: load 16 Y bytes + 8 chroma bytes per row,
        interleave with byte shuffling via vzip/vunzip patterns.
        Full rows of 16+ pixels fill 128-bit registers well.

------------------------------------------------------------------------------*/

void h264bsdConvertImageToPpa(image_t *image)
{
    u32 picWidth, picHeight, picSize;

    if (!image->ppa_data)
        return;

    u32 t_start = H264_GET_TIME_US();

    picWidth = image->width;
    picHeight = image->height;
    picSize = picWidth * picHeight;

    u32 pixWidth = picWidth * 16;
    u32 pixHeight = picHeight * 16;
    u32 uvStride = pixWidth / 2;

    u8 *yPlane = image->data;
    u8 *uPlane = image->data + picSize * 256;
    u8 *vPlane = uPlane + picSize * 64;

    u32 ppaStride = image->ppa_stride;

    for (u32 row = 0; row < pixHeight; row++)
    {
        const u8 *y_row = yPlane + row * pixWidth;
        const u8 *c_row = (row & 1)
            ? vPlane + (row >> 1) * uvStride
            : uPlane + (row >> 1) * uvStride;
        u8 *dst = image->ppa_data + row * ppaStride;

        for (u32 x = 0; x + 7 < pixWidth; x += 8)
        {
            u32 cx = x >> 1;
            dst[0]  = c_row[cx + 0];
            dst[1]  = y_row[x + 0];
            dst[2]  = y_row[x + 1];
            dst[3]  = c_row[cx + 1];
            dst[4]  = y_row[x + 2];
            dst[5]  = y_row[x + 3];
            dst[6]  = c_row[cx + 2];
            dst[7]  = y_row[x + 4];
            dst[8]  = y_row[x + 5];
            dst[9]  = c_row[cx + 3];
            dst[10] = y_row[x + 6];
            dst[11] = y_row[x + 7];
            dst += 12;
        }
    }

    h264bsdTimerAdd(H264_TIMER_CONVERT_PPA, H264_GET_TIME_US() - t_start);
}

