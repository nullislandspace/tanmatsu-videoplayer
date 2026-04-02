/*
 * PC test harness for h264bsd decoder.
 * Decodes raw H.264 Annex B bitstream, writes I420 frames to file.
 * Used for regression testing: output must be byte-identical before/after changes.
 *
 * Usage: ./test_decode <input.h264> <output.i420> [max_frames]
 *   max_frames defaults to 0 (decode all)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h264bsd_decoder.h"
#include "h264bsd_util.h"

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input.h264> <output.i420> [max_frames]\n", argv[0]);
        return 1;
    }

    const char *input_path = argv[1];
    const char *output_path = argv[2];
    u32 max_frames = 0;
    if (argc >= 4)
        max_frames = (u32)atoi(argv[3]);

    /* Read entire input file */
    FILE *fin = fopen(input_path, "rb");
    if (!fin) {
        fprintf(stderr, "Cannot open input: %s\n", input_path);
        return 1;
    }
    fseek(fin, 0, SEEK_END);
    long file_size = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    u8 *input_buf = (u8 *)malloc(file_size);
    if (!input_buf) {
        fprintf(stderr, "Cannot allocate %ld bytes for input\n", file_size);
        fclose(fin);
        return 1;
    }
    if (fread(input_buf, 1, file_size, fin) != (size_t)file_size) {
        fprintf(stderr, "Failed to read input file\n");
        fclose(fin);
        free(input_buf);
        return 1;
    }
    fclose(fin);

    fprintf(stderr, "Input: %s (%ld bytes)\n", input_path, file_size);

    /* Open output file */
    FILE *fout = fopen(output_path, "wb");
    if (!fout) {
        fprintf(stderr, "Cannot open output: %s\n", output_path);
        free(input_buf);
        return 1;
    }

    /* Initialize decoder */
    storage_t *decoder = h264bsdAlloc();
    if (!decoder) {
        fprintf(stderr, "Failed to allocate decoder\n");
        fclose(fout);
        free(input_buf);
        return 1;
    }

    u32 ret = h264bsdInit(decoder, 1); /* noOutputReordering=1 */
    if (ret != HANTRO_OK) {
        fprintf(stderr, "Failed to init decoder: %u\n", ret);
        h264bsdFree(decoder);
        fclose(fout);
        free(input_buf);
        return 1;
    }

    /* Decode loop */
    u8 *remaining = input_buf;
    u32 remaining_len = (u32)file_size;
    u32 frame_count = 0;
    u32 pic_id = 0;
    u32 pic_width = 0, pic_height = 0;
    int decode_errors = 0;

    while (remaining_len > 0) {
        u32 read_bytes = 0;
        ret = h264bsdDecode(decoder, remaining, remaining_len, pic_id, &read_bytes);

        if (read_bytes == 0 && ret != H264BSD_PIC_RDY && ret != H264BSD_HDRS_RDY)
            break;

        remaining += read_bytes;
        remaining_len -= read_bytes;

        switch (ret) {
            case H264BSD_PIC_RDY: {
                u32 out_pic_id = 0, is_idr = 0, num_err_mbs = 0;
                u8 *pic = h264bsdNextOutputPicture(decoder, &out_pic_id, &is_idr, &num_err_mbs);
                if (pic) {
                    if (pic_width == 0) {
                        pic_width = h264bsdPicWidth(decoder) * 16;
                        pic_height = h264bsdPicHeight(decoder) * 16;
                        fprintf(stderr, "Resolution: %ux%u\n", pic_width, pic_height);
                    }
                    /* Write I420 frame: Y + U + V */
                    u32 luma_size = pic_width * pic_height;
                    u32 chroma_size = luma_size / 4;
                    u32 frame_size = luma_size + 2 * chroma_size;
                    fwrite(pic, 1, frame_size, fout);
                    frame_count++;
                    pic_id++;

                    if (frame_count % 100 == 0)
                        fprintf(stderr, "  Decoded %u frames...\n", frame_count);

                    if (max_frames > 0 && frame_count >= max_frames) {
                        remaining_len = 0; /* stop */
                    }
                }
                break;
            }
            case H264BSD_HDRS_RDY: {
                u32 w = h264bsdPicWidth(decoder);
                u32 h = h264bsdPicHeight(decoder);
                if (w && h)
                    fprintf(stderr, "Headers: %ux%u\n", w * 16, h * 16);
                break;
            }
            case H264BSD_RDY:
                break;
            case H264BSD_ERROR:
                decode_errors++;
                break;
            case H264BSD_PARAM_SET_ERROR:
                fprintf(stderr, "Param set error\n");
                decode_errors++;
                break;
            case H264BSD_MEMALLOC_ERROR:
                fprintf(stderr, "Memory allocation error\n");
                remaining_len = 0;
                break;
            default:
                break;
        }
    }

    fprintf(stderr, "Decoded %u frames (%d errors)\n", frame_count, decode_errors);

    h264bsdShutdown(decoder);
    h264bsdFree(decoder);
    fclose(fout);
    free(input_buf);

    return 0;
}
