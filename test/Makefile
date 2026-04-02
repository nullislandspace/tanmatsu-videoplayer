H264BSD_SRC = ../components/h264bsd/src
H264BSD_INC = ../components/h264bsd/src

SRCS = \
	$(H264BSD_SRC)/h264bsd_byte_stream.c \
	$(H264BSD_SRC)/h264bsd_cavlc.c \
	$(H264BSD_SRC)/h264bsd_conceal.c \
	$(H264BSD_SRC)/h264bsd_deblocking.c \
	$(H264BSD_SRC)/h264bsd_decoder.c \
	$(H264BSD_SRC)/h264bsd_dpb.c \
	$(H264BSD_SRC)/h264bsd_image.c \
	$(H264BSD_SRC)/h264bsd_inter_prediction.c \
	$(H264BSD_SRC)/h264bsd_intra_prediction.c \
	$(H264BSD_SRC)/h264bsd_macroblock_layer.c \
	$(H264BSD_SRC)/h264bsd_nal_unit.c \
	$(H264BSD_SRC)/h264bsd_neighbour.c \
	$(H264BSD_SRC)/h264bsd_pic_order_cnt.c \
	$(H264BSD_SRC)/h264bsd_pic_param_set.c \
	$(H264BSD_SRC)/h264bsd_reconstruct.c \
	$(H264BSD_SRC)/h264bsd_sei.c \
	$(H264BSD_SRC)/h264bsd_seq_param_set.c \
	$(H264BSD_SRC)/h264bsd_slice_data.c \
	$(H264BSD_SRC)/h264bsd_slice_group_map.c \
	$(H264BSD_SRC)/h264bsd_slice_header.c \
	$(H264BSD_SRC)/h264bsd_storage.c \
	$(H264BSD_SRC)/h264bsd_stream.c \
	$(H264BSD_SRC)/h264bsd_transform.c \
	$(H264BSD_SRC)/h264bsd_util.c \
	$(H264BSD_SRC)/h264bsd_vlc.c \
	$(H264BSD_SRC)/h264bsd_vui.c \
	test_decode.c

CC = gcc
CFLAGS = -O2 -Wall -Wno-unused-variable -Wno-unused-but-set-variable -I$(H264BSD_INC)

test_decode: $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f test_decode *_test.i420

.PHONY: clean
