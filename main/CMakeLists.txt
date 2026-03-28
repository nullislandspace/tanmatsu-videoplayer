idf_component_register(
	SRCS
		"main.c"
		"file_browser.c"
		"mjpeg_decoder.c"
		"avi_parser.c"
		"audio_player.c"
		"usb_device.c"
		"sdcard.c"
		"fastopen.c"
	PRIV_REQUIRES
		esp_lcd
		badge-bsp
		fatfs
		sdmmc
		esp_driver_sdmmc
		esp_driver_sdspi
		esp_driver_spi
		esp_driver_i2s
		esp_driver_jpeg
		vfs
	INCLUDE_DIRS
		"."
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
