set(COMMON_SRC "webrtc.cpp" "main.cpp" "http.cpp")

if(IDF_TARGET STREQUAL linux)
	idf_component_register(
		SRCS ${COMMON_SRC}
		REQUIRES peer esp-libopus esp_http_client)
else()
	idf_component_register(
		SRCS ${COMMON_SRC} "lcd.cpp" "wifi_config.cpp" "media.cpp" "aipi_lite_config.c" "es8311.c"
		REQUIRES driver esp_wifi nvs_flash peer esp_psram esp-libopus esp_http_client esp_https_server json esp32_s3_touch_amoled_1_8 esp_lcd_touch esp_io_expander esp_io_expander_tca9554)
endif()

idf_component_get_property(lib peer COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wno-error=restrict)
target_compile_options(${lib} PRIVATE -Wno-error=stringop-truncation)

idf_component_get_property(lib srtp COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wno-error=incompatible-pointer-types)

idf_component_get_property(lib esp-libopus COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wno-error=maybe-uninitialized)
target_compile_options(${lib} PRIVATE -Wno-error=stringop-overread)
