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

                    /* Verify PPA output against reference conversion */
                    u32 ppa_stride = 0;
                    u8 *ppa = h264bsdNextOutputPicturePpa(decoder, &ppa_stride);
                    if (ppa && ppa_stride > 0) {
                        u8 *y_plane = pic;
                        u8 *u_plane = pic + luma_size;
                        u8 *v_plane = u_plane + chroma_size;
                        u32 uv_stride = pic_width / 2;
                        int ppa_ok = 1;

                        for (u32 r = 0; r < pic_height && ppa_ok; r++) {
                            const u8 *y_row = y_plane + r * pic_width;
                            const u8 *c_row = (r & 1)
                                ? v_plane + (r >> 1) * uv_stride
                                : u_plane + (r >> 1) * uv_stride;
                            u8 *ppa_row = ppa + r * ppa_stride;

                            for (u32 x = 0; x + 7 < pic_width; x += 8) {
                                u32 cx = x >> 1;
                                u8 ref[12];
                                ref[0]  = c_row[cx + 0];
                                ref[1]  = y_row[x + 0];
                                ref[2]  = y_row[x + 1];
                                ref[3]  = c_row[cx + 1];
                                ref[4]  = y_row[x + 2];
                                ref[5]  = y_row[x + 3];
                                ref[6]  = c_row[cx + 2];
                                ref[7]  = y_row[x + 4];
                                ref[8]  = y_row[x + 5];
                                ref[9]  = c_row[cx + 3];
                                ref[10] = y_row[x + 6];
                                ref[11] = y_row[x + 7];

                                u8 *ppa_pos = ppa_row + (x * 3 / 2);
                                if (memcmp(ref, ppa_pos, 12) != 0) {
                                    if (ppa_ok) { /* only print first mismatch per frame */
                                        fprintf(stderr, "PPA MISMATCH: frame %u, row %u, x %u\n",
                                                frame_count, r, x);
                                        fprintf(stderr, "  ref: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                                ref[0],ref[1],ref[2],ref[3],ref[4],ref[5],
                                                ref[6],ref[7],ref[8],ref[9],ref[10],ref[11]);
                                        fprintf(stderr, "  got: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                                ppa_pos[0],ppa_pos[1],ppa_pos[2],ppa_pos[3],
                                                ppa_pos[4],ppa_pos[5],ppa_pos[6],ppa_pos[7],
                                                ppa_pos[8],ppa_pos[9],ppa_pos[10],ppa_pos[11]);
                                    }
                                    ppa_ok = 0;
                                }
                            }
                        }
                        if (ppa_ok && frame_count == 0)
                            fprintf(stderr, "PPA verification: OK (first frame)\n");
                    }

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
