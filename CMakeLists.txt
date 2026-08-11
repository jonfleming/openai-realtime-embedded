cmake_minimum_required(VERSION 3.19)

if(IDF_TARGET STREQUAL linux)
  file(READ privateConfig.json PV_CFG)
  string(JSON PV_WIFI_SSID GET "${PV_CFG}" wifi_ssid)
  string(JSON PV_WIFI_PASSWORD GET "${PV_CFG}" wifi_password)
  string(JSON PV_OPENAI_API_KEY GET "${PV_CFG}" openai_api_key)
  set(ENV{WIFI_SSID} ${PV_WIFI_SSID})
  set(ENV{WIFI_PASSWORD} ${PV_WIFI_PASSWORD})
  set(ENV{OPENAI_API_KEY} ${PV_OPENAI_API_KEY})
  add_compile_definitions(WIFI_SSID="$ENV{WIFI_SSID}")
  add_compile_definitions(WIFI_PASSWORD="$ENV{WIFI_PASSWORD}")
  add_compile_definitions(LOG_DATACHANNEL_MESSAGES="1")
  if(NOT DEFINED ENV{OPENAI_API_KEY})
    message(FATAL_ERROR "Env variable OPENAI_API_KEY must be set")
  endif()
  add_compile_definitions(OPENAI_API_KEY="$ENV{OPENAI_API_KEY}")
	add_compile_definitions(LINUX_BUILD=1)
  list(APPEND EXTRA_COMPONENT_DIRS
    $ENV{IDF_PATH}/examples/protocols/linux_stubs/esp_stubs
    "components/esp-protocols/common_components/linux_compat/esp_timer"
    "components/esp-protocols/common_components/linux_compat/freertos"
    )
endif()


# Board support (mutually exclusive, precedence Waveshare > AIPI-Lite > Freenove):
#   -DWAVESHARE_AMOLED_1_8_BOARD=ON -> Waveshare ESP32-S3 Touch AMOLED 1.8
#   -DAIPI_LITE_BOARD=ON            -> AIPI-Lite (default)
#   -DAIPI_LITE_BOARD=OFF           -> Freenove Media Kit
option(WAVESHARE_AMOLED_1_8_BOARD "Build for Waveshare ESP32-S3 Touch AMOLED 1.8" ON)
option(AIPI_LITE_BOARD "Build for AIPI-Lite board" OFF)
if(WAVESHARE_AMOLED_1_8_BOARD)
  add_compile_definitions(WAVESHARE_AMOLED_1_8_BOARD=1)
  add_compile_definitions(AIPI_LITE_BOARD=0)
elseif(AIPI_LITE_BOARD)
  add_compile_definitions(WAVESHARE_AMOLED_1_8_BOARD=0)
  add_compile_definitions(AIPI_LITE_BOARD=1)
else()
  add_compile_definitions(WAVESHARE_AMOLED_1_8_BOARD=0)
  add_compile_definitions(AIPI_LITE_BOARD=0)
endif()

add_compile_definitions(OPENAI_REALTIMEAPI="https://speech.techion.net/v1/realtime")

# Waveshare board-specific sdkconfig overlay (USB-Serial-JTAG console, etc.)
if(WAVESHARE_AMOLED_1_8_BOARD)
  set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.waveshare_amoled_1_8")
endif()

set(COMPONENTS src)
set(EXTRA_COMPONENT_DIRS "src" "components/srtp" "components/peer" "components/esp-libopus")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(src)
