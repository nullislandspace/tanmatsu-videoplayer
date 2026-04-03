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
          h264bsdDecodeMacroblockLayer
          h264bsdMbPartPredMode
          h264bsdNumMbPart
          h264bsdNumSubMbPart
          DecodeMbPred
          DecodeSubMbPred
          DecodeResidual
          DetermineNc
          CbpIntra16x16
          h264bsdPredModeIntra16x16
          h264bsdDecodeMacroblock
          ProcessResidual
          h264bsdSubMbPartMode

------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "h264bsd_macroblock_layer.h"
#include "h264bsd_slice_header.h"
#include "h264bsd_util.h"
#include "h264bsd_vlc.h"
#include "h264bsd_cavlc.h"
#include "h264bsd_nal_unit.h"
#include "h264bsd_neighbour.h"
#include "h264bsd_transform.h"
#include "h264bsd_intra_prediction.h"
#include "h264bsd_inter_prediction.h"
#include "h264bsd_profile.h"

/*------------------------------------------------------------------------------
    2. External compiler flags
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
    3. Module defines
------------------------------------------------------------------------------*/
/* mapping of dc coefficients array to luma blocks */
static const u32 dcCoeffIndex[16] =
    {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};

/*------------------------------------------------------------------------------
    4. Local function prototypes
------------------------------------------------------------------------------*/

static u32 DecodeMbPred(strmData_t *pStrmData, mbPred_t *pMbPred,
    mbType_e mbType, u32 numRefIdxActive);
static u32 DecodeSubMbPred(strmData_t *pStrmData, subMbPred_t *pSubMbPred,
    mbType_e mbType, u32 numRefIdxActive);
static u32 DecodeResidual(strmData_t *pStrmData, residual_t *pResidual,
    mbStorage_t *pMb, mbType_e mbType, u32 codedBlockPattern);

static u32 DetermineNc(mbStorage_t *pMb, u32 blockIndex, i16 *pTotalCoeff);

static u32 CbpIntra16x16(mbType_e mbType);
static u32 ProcessResidual(mbStorage_t *pMb, residual_t *pResidual);

/*------------------------------------------------------------------------------

    Function name: h264bsdDecodeMacroblockLayer

        Functional description:
          Parse macroblock specific information from bit stream.

        Inputs:
          pStrmData         pointer to stream data structure
          pMb               pointer to macroblock storage structure
          sliceType         type of the current slice
          numRefIdxActive   maximum reference index

        Outputs:
          pMbLayer          stores the macroblock data parsed from stream

        Returns:
          HANTRO_OK         success
          HANTRO_NOK        end of stream or error in stream

------------------------------------------------------------------------------*/

u32 h264bsdDecodeMacroblockLayer(strmData_t *pStrmData,
    macroblockLayer_t *pMbLayer, mbStorage_t *pMb, u32 sliceType,
    u32 numRefIdxActive)
{

/* Variables */

    u32 tmp, i, value;
    i32 itmp;
    mbPartPredMode_e partMode;

/* Code */

    ASSERT(pStrmData);
    ASSERT(pMbLayer);

    memset(pMbLayer, 0, sizeof(macroblockLayer_t));

    tmp = h264bsdDecodeExpGolombUnsigned(pStrmData, &value);

    if (IS_I_SLICE(sliceType))
    {
        if ((value + 6) > 31 || tmp != HANTRO_OK)
            return(HANTRO_NOK);
        pMbLayer->mbType = (mbType_e)(value + 6);
    }
    else
    {
        if ((value + 1) > 31 || tmp != HANTRO_OK)
            return(HANTRO_NOK);
        pMbLayer->mbType = (mbType_e)(value + 1);
    }

    if (pMbLayer->mbType == I_PCM)
    {
        i32 *level;
        while( !h264bsdIsByteAligned(pStrmData) )
        {
            /* pcm_alignment_zero_bit */
            tmp = h264bsdGetBits(pStrmData, 1);
            if (tmp)
                return(HANTRO_NOK);
        }

        level = pMbLayer->residual.level[0];
        for (i = 0; i < 384; i++)
        {
            value = h264bsdGetBits(pStrmData, 8);
            if (value == END_OF_STREAM)
                return(HANTRO_NOK);
            *level++ = (i32)value;
        }
    }
    else
    {
        partMode = h264bsdMbPartPredMode(pMbLayer->mbType);
        if ( (partMode == PRED_MODE_INTER) &&
             (h264bsdNumMbPart(pMbLayer->mbType) == 4) )
        {
            tmp = DecodeSubMbPred(pStrmData, &pMbLayer->subMbPred,
                pMbLayer->mbType, numRefIdxActive);
        }
        else
        {
            tmp = DecodeMbPred(pStrmData, &pMbLayer->mbPred,
                pMbLayer->mbType, numRefIdxActive);
        }
        if (tmp != HANTRO_OK)
            return(tmp);

        if (partMode != PRED_MODE_INTRA16x16)
        {
            tmp = h264bsdDecodeExpGolombMapped(pStrmData, &value,
                (u32)(partMode == PRED_MODE_INTRA4x4));
            if (tmp != HANTRO_OK)
                return(tmp);
            pMbLayer->codedBlockPattern = value;
        }
        else
        {
            pMbLayer->codedBlockPattern = CbpIntra16x16(pMbLayer->mbType);
        }

        if ( pMbLayer->codedBlockPattern ||
             (partMode == PRED_MODE_INTRA16x16) )
        {
            tmp = h264bsdDecodeExpGolombSigned(pStrmData, &itmp);
            if (tmp != HANTRO_OK || (itmp < -26) || (itmp > 25) )
                return(HANTRO_NOK);
            pMbLayer->mbQpDelta = itmp;

            tmp = DecodeResidual(pStrmData, &pMbLayer->residual, pMb,
                pMbLayer->mbType, pMbLayer->codedBlockPattern);

            pStrmData->strmBuffReadBits =
                (u32)(pStrmData->pStrmCurrPos - pStrmData->pStrmBuffStart) * 8 +
                pStrmData->bitPosInWord;

            if (tmp != HANTRO_OK)
                return(tmp);
        }
    }

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

    Function: h264bsdMbPartPredMode

        Functional description:
          Returns the prediction mode of a macroblock type

------------------------------------------------------------------------------*/

mbPartPredMode_e h264bsdMbPartPredMode(mbType_e mbType)
{

/* Variables */


/* Code */

    ASSERT(mbType <= 31);

    if ((mbType <= P_8x8ref0))
        return(PRED_MODE_INTER);
    else if (mbType == I_4x4)
        return(PRED_MODE_INTRA4x4);
    else
        return(PRED_MODE_INTRA16x16);

}

/*------------------------------------------------------------------------------

    Function: h264bsdNumMbPart

        Functional description:
          Returns the amount of macroblock partitions in a macroblock type

------------------------------------------------------------------------------*/

u32 h264bsdNumMbPart(mbType_e mbType)
{

/* Variables */


/* Code */

    ASSERT(h264bsdMbPartPredMode(mbType) == PRED_MODE_INTER);

    switch (mbType)
    {
        case P_L0_16x16:
        case P_Skip:
            return(1);

        case P_L0_L0_16x8:
        case P_L0_L0_8x16:
            return(2);

        /* P_8x8 or P_8x8ref0 */
        default:
            return(4);
    }

}

/*------------------------------------------------------------------------------

    Function: h264bsdNumSubMbPart

        Functional description:
          Returns the amount of sub-partitions in a sub-macroblock type

------------------------------------------------------------------------------*/

u32 h264bsdNumSubMbPart(subMbType_e subMbType)
{

/* Variables */


/* Code */

    ASSERT(subMbType <= P_L0_4x4);

    switch (subMbType)
    {
        case P_L0_8x8:
            return(1);

        case P_L0_8x4:
        case P_L0_4x8:
            return(2);

        /* P_L0_4x4 */
        default:
            return(4);
    }

}

/*------------------------------------------------------------------------------

    Function: DecodeMbPred

        Functional description:
          Parse macroblock prediction information from bit stream and store
          in 'pMbPred'.

------------------------------------------------------------------------------*/

u32 DecodeMbPred(strmData_t *pStrmData, mbPred_t *pMbPred, mbType_e mbType,
    u32 numRefIdxActive)
{

/* Variables */

    u32 tmp, i, j, value;
    i32 itmp;

/* Code */

    ASSERT(pStrmData);
    ASSERT(pMbPred);

    switch (h264bsdMbPartPredMode(mbType))
    {
        case PRED_MODE_INTER: /* PRED_MODE_INTER */
            if (numRefIdxActive > 1)
            {
                for (i = h264bsdNumMbPart(mbType), j = 0; i--;  j++)
                {
                    tmp = h264bsdDecodeExpGolombTruncated(pStrmData, &value,
                        (u32)(numRefIdxActive > 2));
                    if (tmp != HANTRO_OK || value >= numRefIdxActive)
                        return(HANTRO_NOK);

                    pMbPred->refIdxL0[j] = value;
                }
            }

            for (i = h264bsdNumMbPart(mbType), j = 0; i--;  j++)
            {
                tmp = h264bsdDecodeExpGolombSigned(pStrmData, &itmp);
                if (tmp != HANTRO_OK)
                    return(tmp);
                pMbPred->mvdL0[j].hor = (i16)itmp;

                tmp = h264bsdDecodeExpGolombSigned(pStrmData, &itmp);
                if (tmp != HANTRO_OK)
                    return(tmp);
                pMbPred->mvdL0[j].ver = (i16)itmp;
            }
            break;

        case PRED_MODE_INTRA4x4:
            for (itmp = 0, i = 0; itmp < 2; itmp++)
            {
                value = h264bsdShowBits32(pStrmData);
                tmp = 0;
                for (j = 8; j--; i++)
                {
                    pMbPred->prevIntra4x4PredModeFlag[i] =
                        value & 0x80000000 ? HANTRO_TRUE : HANTRO_FALSE;
                    value <<= 1;
                    if (!pMbPred->prevIntra4x4PredModeFlag[i])
                    {
                        pMbPred->remIntra4x4PredMode[i] = value>>29;
                        value <<= 3;
                        tmp++;
                    }
                }
                if (h264bsdFlushBits(pStrmData, 8 + 3*tmp) == END_OF_STREAM)
                    return(HANTRO_NOK);
            }
            /* fall-through */

        case PRED_MODE_INTRA16x16:
            tmp = h264bsdDecodeExpGolombUnsigned(pStrmData, &value);
            if (tmp != HANTRO_OK || value > 3)
                return(HANTRO_NOK);
            pMbPred->intraChromaPredMode = value;
            break;
    }

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

    Function: DecodeSubMbPred

        Functional description:
          Parse sub-macroblock prediction information from bit stream and
          store in 'pMbPred'.

------------------------------------------------------------------------------*/

u32 DecodeSubMbPred(strmData_t *pStrmData, subMbPred_t *pSubMbPred,
    mbType_e mbType, u32 numRefIdxActive)
{

/* Variables */

    u32 tmp, i, j, value;
    i32 itmp;

/* Code */

    ASSERT(pStrmData);
    ASSERT(pSubMbPred);
    ASSERT(h264bsdMbPartPredMode(mbType) == PRED_MODE_INTER);

    for (i = 0; i < 4; i++)
    {
        tmp = h264bsdDecodeExpGolombUnsigned(pStrmData, &value);
        if (tmp != HANTRO_OK || value > 3)
            return(HANTRO_NOK);
        pSubMbPred->subMbType[i] = (subMbType_e)value;
    }

    if ( (numRefIdxActive > 1) && (mbType != P_8x8ref0) )
    {
        for (i = 0; i < 4; i++)
        {
            tmp = h264bsdDecodeExpGolombTruncated(pStrmData, &value,
                (u32)(numRefIdxActive > 2));
            if (tmp != HANTRO_OK || value >= numRefIdxActive)
                return(HANTRO_NOK);
            pSubMbPred->refIdxL0[i] = value;
        }
    }

    for (i = 0; i < 4; i++)
    {
        j = 0;
        for (value = h264bsdNumSubMbPart(pSubMbPred->subMbType[i]);
             value--; j++)
        {
            tmp = h264bsdDecodeExpGolombSigned(pStrmData, &itmp);
            if (tmp != HANTRO_OK)
                return(tmp);
            pSubMbPred->mvdL0[i][j].hor = (i16)itmp;

            tmp = h264bsdDecodeExpGolombSigned(pStrmData, &itmp);
            if (tmp != HANTRO_OK)
                return(tmp);
            pSubMbPred->mvdL0[i][j].ver = (i16)itmp;
        }
    }

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

    Function: DecodeResidual

        Functional description:
          Parse residual information from bit stream and store in 'pResidual'.

------------------------------------------------------------------------------*/

u32 DecodeResidual(strmData_t *pStrmData, residual_t *pResidual,
    mbStorage_t *pMb, mbType_e mbType, u32 codedBlockPattern)
{

/* Variables */

    u32 i, j, tmp;
    i32 nc;
    u32 blockCoded;
    u32 blockIndex;
    u32 is16x16;
    i32 (*level)[16];

/* Code */

    ASSERT(pStrmData);
    ASSERT(pResidual);

    level = pResidual->level;

    /* luma DC is at index 24 */
    if (h264bsdMbPartPredMode(mbType) == PRED_MODE_INTRA16x16)
    {
        nc = (i32)DetermineNc(pMb, 0, pResidual->totalCoeff);
        tmp = h264bsdDecodeResidualBlockCavlc(pStrmData, level[24], nc, 16);
        if ((tmp & 0xF) != HANTRO_OK)
            return(tmp);
        pResidual->totalCoeff[24] = (tmp >> 4) & 0xFF;
        is16x16 = HANTRO_TRUE;
    }
    else
        is16x16 = HANTRO_FALSE;

    for (i = 4, blockIndex = 0; i--;)
    {
        /* luma cbp in bits 0-3 */
        blockCoded = codedBlockPattern & 0x1;
        codedBlockPattern >>= 1;
        if (blockCoded)
        {
            for (j = 4; j--; blockIndex++)
            {
                nc = (i32)DetermineNc(pMb, blockIndex, pResidual->totalCoeff);
                if (is16x16)
                {
                    tmp = h264bsdDecodeResidualBlockCavlc(pStrmData,
                        level[blockIndex] + 1, nc, 15);
                    pResidual->coeffMap[blockIndex] = tmp >> 15;
                }
                else
                {
                    tmp = h264bsdDecodeResidualBlockCavlc(pStrmData,
                        level[blockIndex], nc, 16);
                    pResidual->coeffMap[blockIndex] = tmp >> 16;
                }
                if ((tmp & 0xF) != HANTRO_OK)
                    return(tmp);
                pResidual->totalCoeff[blockIndex] = (tmp >> 4) & 0xFF;
            }
        }
        else
            blockIndex += 4;
    }

    /* chroma DC block are at indices 25 and 26 */
    blockCoded = codedBlockPattern & 0x3;
    if (blockCoded)
    {
        tmp = h264bsdDecodeResidualBlockCavlc(pStrmData, level[25], -1, 4);
        if ((tmp & 0xF) != HANTRO_OK)
            return(tmp);
        pResidual->totalCoeff[25] = (tmp >> 4) & 0xFF;
        tmp = h264bsdDecodeResidualBlockCavlc(pStrmData, level[25]+4, -1, 4);
        if ((tmp & 0xF) != HANTRO_OK)
            return(tmp);
        pResidual->totalCoeff[26] = (tmp >> 4) & 0xFF;
    }

    /* chroma AC */
    blockCoded = codedBlockPattern & 0x2;
    if (blockCoded)
    {
        for (i = 8; i--;blockIndex++)
        {
            nc = (i32)DetermineNc(pMb, blockIndex, pResidual->totalCoeff);
            tmp = h264bsdDecodeResidualBlockCavlc(pStrmData,
                level[blockIndex] + 1, nc, 15);
            if ((tmp & 0xF) != HANTRO_OK)
                return(tmp);
            pResidual->totalCoeff[blockIndex] = (tmp >> 4) & 0xFF;
            pResidual->coeffMap[blockIndex] = (tmp >> 15);
        }
    }

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

    Function: DetermineNc

        Functional description:
          Returns the nC of a block.

------------------------------------------------------------------------------*/
u32 DetermineNc(mbStorage_t *pMb, u32 blockIndex, i16 *pTotalCoeff)
{
/*lint -e702 */
/* Variables */

    u32 tmp;
    i32 n;
    const neighbour_t *neighbourA, *neighbourB;
    u8 neighbourAindex, neighbourBindex;

/* Code */

    ASSERT(blockIndex < 24);

    /* if neighbour block belongs to current macroblock totalCoeff array
     * mbStorage has not been set/updated yet -> use pTotalCoeff */
    neighbourA = h264bsdNeighbour4x4BlockA(blockIndex);
    neighbourB = h264bsdNeighbour4x4BlockB(blockIndex);
    neighbourAindex = neighbourA->index;
    neighbourBindex = neighbourB->index;
    if (neighbourA->mb == MB_CURR && neighbourB->mb == MB_CURR)
    {
        n = (pTotalCoeff[neighbourAindex] +
             pTotalCoeff[neighbourBindex] + 1)>>1;
    }
    else if (neighbourA->mb == MB_CURR)
    {
        n = pTotalCoeff[neighbourAindex];
        if (h264bsdIsNeighbourAvailable(pMb, pMb->mbB))
        {
            n = (n + pMb->mbB->totalCoeff[neighbourBindex] + 1) >> 1;
        }
    }
    else if (neighbourB->mb == MB_CURR)
    {
        n = pTotalCoeff[neighbourBindex];
        if (h264bsdIsNeighbourAvailable(pMb, pMb->mbA))
        {
            n = (n + pMb->mbA->totalCoeff[neighbourAindex] + 1) >> 1;
        }
    }
    else
    {
        n = tmp = 0;
        if (h264bsdIsNeighbourAvailable(pMb, pMb->mbA))
        {
            n = pMb->mbA->totalCoeff[neighbourAindex];
            tmp = 1;
        }
        if (h264bsdIsNeighbourAvailable(pMb, pMb->mbB))
        {
            if (tmp)
                n = (n + pMb->mbB->totalCoeff[neighbourBindex] + 1) >> 1;
            else
                n = pMb->mbB->totalCoeff[neighbourBindex];
        }
    }
    return((u32)n);
/*lint +e702 */
}

/*------------------------------------------------------------------------------

    Function: CbpIntra16x16

        Functional description:
          Returns the coded block pattern for intra 16x16 macroblock.

------------------------------------------------------------------------------*/

u32 CbpIntra16x16(mbType_e mbType)
{

/* Variables */

    u32 cbp;
    u32 tmp;

/* Code */

    ASSERT(mbType >= I_16x16_0_0_0 && mbType <= I_16x16_3_2_1);

    if (mbType >= I_16x16_0_0_1)
        cbp = 15;
    else
        cbp = 0;

    /* tmp is 0 for I_16x16_0_0_0 mb type */
    /* ignore lint warning on arithmetic on enum's */
    tmp = /*lint -e(656)*/(mbType - I_16x16_0_0_0) >> 2;
    if (tmp > 2)
        tmp -= 3;

    cbp += tmp << 4;

    return(cbp);

}

/*------------------------------------------------------------------------------

    Function: h264bsdPredModeIntra16x16

        Functional description:
          Returns the prediction mode for intra 16x16 macroblock.

------------------------------------------------------------------------------*/

u32 h264bsdPredModeIntra16x16(mbType_e mbType)
{

/* Variables */

    u32 tmp;

/* Code */

    ASSERT(mbType >= I_16x16_0_0_0 && mbType <= I_16x16_3_2_1);

    /* tmp is 0 for I_16x16_0_0_0 mb type */
    /* ignore lint warning on arithmetic on enum's */
    tmp = /*lint -e(656)*/(mbType - I_16x16_0_0_0);

    return(tmp & 0x3);

}

/*------------------------------------------------------------------------------

    Function: h264bsdDecodeMacroblock

        Functional description:
          Decode one macroblock and write into output image.

        Inputs:
          pMb           pointer to macroblock specific information
          mbLayer       pointer to current macroblock data from stream
          currImage     pointer to output image
          dpb           pointer to decoded picture buffer
          qpY           pointer to slice QP
          mbNum         current macroblock number
          constrainedIntraPred  flag specifying if neighbouring inter
                                macroblocks are used in intra prediction

        Outputs:
          pMb           structure is updated with current macroblock
          currImage     decoded macroblock is written into output image

        Returns:
          HANTRO_OK     success
          HANTRO_NOK    error in macroblock decoding

------------------------------------------------------------------------------*/

u32 h264bsdDecodeMacroblock(mbStorage_t *pMb, macroblockLayer_t *pMbLayer,
    image_t *currImage, dpbStorage_t *dpb, i32 *qpY, u32 mbNum,
    u32 constrainedIntraPredFlag, u8* data)
{

/* Variables */

    u32 i, tmp;
    mbType_e mbType;

/* Code */

    ASSERT(pMb);
    ASSERT(pMbLayer);
    ASSERT(currImage);
    ASSERT(qpY && *qpY < 52);
    ASSERT(mbNum < currImage->width*currImage->height);

    mbType = pMbLayer->mbType;
    pMb->mbType = mbType;

    pMb->decoded++;

    h264bsdSetCurrImageMbPointers(currImage, mbNum);

    if (mbType == I_PCM)
    {
        u8 *pData = (u8*)data;
        i16 *tot = pMb->totalCoeff;
        i32 *lev = pMbLayer->residual.level[0];

        pMb->qpY = 0;

        /* if decoded flag > 1 -> mb has already been successfully decoded and
         * written to output -> do not write again */
        if (pMb->decoded > 1)
        {
            for (i = 24; i--;)
                *tot++ = 16;
            return HANTRO_OK;
        }

        for (i = 24; i--;)
        {
            *tot++ = 16;
            for (tmp = 16; tmp--;)
                *pData++ = (u8)(*lev++);
        }
        h264bsdWriteMacroblock(currImage, (u8*)data);

        return(HANTRO_OK);
    }
    else
    {
        if (mbType != P_Skip)
        {
            memcpy(pMb->totalCoeff,
                            pMbLayer->residual.totalCoeff,
                            27*sizeof(*pMb->totalCoeff));

            /* update qpY */
            if (pMbLayer->mbQpDelta)
            {
                *qpY = *qpY + pMbLayer->mbQpDelta;
                if (*qpY < 0) *qpY += 52;
                else if (*qpY >= 52) *qpY -= 52;
            }
            pMb->qpY = (u32)*qpY;

            {
                u32 t0 = H264_GET_TIME_US();
                tmp = ProcessResidual(pMb, &pMbLayer->residual);
                h264bsdTimerAdd(H264_TIMER_PROCESS_RESIDUAL, H264_GET_TIME_US() - t0);
            }
            if (tmp != HANTRO_OK)
                return (tmp);
        }
        else
        {
            memset(pMb->totalCoeff, 0, 27*sizeof(*pMb->totalCoeff));
            pMb->qpY = (u32)*qpY;
        }
        if (h264bsdMbPartPredMode(mbType) != PRED_MODE_INTER)
        {
            u32 t0 = H264_GET_TIME_US();
            tmp = h264bsdIntraPrediction(pMb, pMbLayer, currImage, mbNum,
                constrainedIntraPredFlag, (u8*)data);
            h264bsdTimerAdd(H264_TIMER_INTRA_PREDICTION, H264_GET_TIME_US() - t0);
            if (tmp != HANTRO_OK) return (tmp);
        }
        else
        {
            u32 t0 = H264_GET_TIME_US();
            tmp = h264bsdInterPrediction(pMb, pMbLayer, dpb, mbNum,
                currImage, (u8*)data);
            h264bsdTimerAdd(H264_TIMER_INTER_PREDICTION, H264_GET_TIME_US() - t0);
            if (tmp != HANTRO_OK) return (tmp);
        }
    }

    return HANTRO_OK;
}


/*------------------------------------------------------------------------------

    Function: ProcessResidual

        Functional description:
          Process the residual data of one macroblock with
          inverse quantization and inverse transform.

------------------------------------------------------------------------------*/

u32 ProcessResidual(mbStorage_t *pMb, residual_t *pResidual)
{

/* Variables */

    u32 i, j, blockIdx;
    u32 chromaQp;
    i32 (*blockData)[16];
    i32 (*blockDc)[16];
    i16 *totalCoeff;
    i32 *chromaDc;
    const u32 *dcCoeffIdx;
    i32 (*residualLevel)[16] = pResidual->level;
    u32 *coeffMap = pResidual->coeffMap;

/* Code */

    ASSERT(pMb);
    ASSERT(pResidual);

    /* set pointers to DC coefficient blocks */
    blockDc = residualLevel + 24;

    blockData = residualLevel;
    totalCoeff = pMb->totalCoeff;
    blockIdx = 0;
    if (h264bsdMbPartPredMode(pMb->mbType) == PRED_MODE_INTRA16x16)
    {
        if (totalCoeff[24])
        {
            h264bsdProcessLumaDc(*blockDc, pMb->qpY);
        }
        dcCoeffIdx = dcCoeffIndex;

        for (i = 16; i--; blockData++, totalCoeff++, coeffMap++, blockIdx++)
        {
            /* set dc coefficient of luma block */
            (*blockData)[0] = (*blockDc)[*dcCoeffIdx++];
            if ((*blockData)[0] || *totalCoeff)
            {
                if (h264bsdProcessBlock(*blockData, pMb->qpY, 1, *coeffMap) !=
                    HANTRO_OK)
                    return(HANTRO_NOK);
                /* narrow i32 → i16 (transform output guaranteed in [-512, 511]) */
                for (j = 0; j < 16; j++)
                    pResidual->levelI16[blockIdx][j] = (i16)(*blockData)[j];
            }
            else
            {
                MARK_RESIDUAL_EMPTY(*blockData);
                MARK_RESIDUAL_EMPTY_I16(pResidual->levelI16[blockIdx]);
            }
        }
    }
    else
    {
        for (i = 16; i--; blockData++, totalCoeff++, coeffMap++, blockIdx++)
        {
            if (*totalCoeff)
            {
                if (h264bsdProcessBlock(*blockData, pMb->qpY, 0, *coeffMap) !=
                    HANTRO_OK)
                    return(HANTRO_NOK);
                for (j = 0; j < 16; j++)
                    pResidual->levelI16[blockIdx][j] = (i16)(*blockData)[j];
            }
            else
            {
                MARK_RESIDUAL_EMPTY(*blockData);
                MARK_RESIDUAL_EMPTY_I16(pResidual->levelI16[blockIdx]);
            }
        }
    }

    /* chroma DC processing. First chroma dc block is block with index 25 */
    chromaQp =
        h264bsdQpC[CLIP3(0, 51, (i32)pMb->qpY + pMb->chromaQpIndexOffset)];
    if (pMb->totalCoeff[25] || pMb->totalCoeff[26])
        h264bsdProcessChromaDc(residualLevel[25], chromaQp);
    chromaDc = residualLevel[25];
    for (i = 8; i--; blockData++, totalCoeff++, coeffMap++, blockIdx++)
    {
        /* set dc coefficient of chroma block */
        (*blockData)[0] = *chromaDc++;
        if ((*blockData)[0] || *totalCoeff)
        {
            if (h264bsdProcessBlock(*blockData, chromaQp, 1,*coeffMap) !=
                HANTRO_OK)
                return(HANTRO_NOK);
            for (j = 0; j < 16; j++)
                pResidual->levelI16[blockIdx][j] = (i16)(*blockData)[j];
        }
        else
        {
            MARK_RESIDUAL_EMPTY(*blockData);
            MARK_RESIDUAL_EMPTY_I16(pResidual->levelI16[blockIdx]);
        }
    }

    return(HANTRO_OK);
}

/*------------------------------------------------------------------------------

    Function: h264bsdSubMbPartMode

        Functional description:
          Returns the macroblock's sub-partition mode.

------------------------------------------------------------------------------*/

subMbPartMode_e h264bsdSubMbPartMode(subMbType_e subMbType)
{

/* Variables */


/* Code */

    ASSERT(subMbType < 4);

    return((subMbPartMode_e)subMbType);

}


