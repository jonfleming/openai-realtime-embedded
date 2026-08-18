cmake_minimum_required(VERSION 3.19)

# This project only targets the ESP32-S3, but the IDF default target is esp32.
# Normally the target is picked up from an existing sdkconfig; a fresh build
# dir (or a deleted sdkconfig) would silently fall back to esp32 and make the
# component manager discard every board BSP (all esp32s3-only) with a confusing
# "no versions match" solve error. Pin the target here, guarded so an explicit
# IDF_TARGET (e.g. "linux" for the Linux build below) still wins.
if(NOT DEFINED IDF_TARGET)
  set(IDF_TARGET esp32s3)
endif()

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


# Board support (mutually exclusive, precedence Waveshare 2.06 > Freenove >
# AIPI-Lite; **Waveshare 1.8 is the default**):
#   -DWAVESHARE_AMOLED_2_06_BOARD=ON -> Waveshare ESP32-S3 Touch AMOLED 2.06 (watch)
#   -DWAVESHARE_AMOLED_1_8_BOARD=ON  -> Waveshare ESP32-S3 Touch AMOLED 1.8 (default, no flag needed)
#   -DAIPI_LITE_BOARD=ON             -> AIPI-Lite
#   -DAIPI_LITE_BOARD=OFF            -> Freenove Media Kit
#
# The two Waveshare boards use different managed BSPs that export the same
# bsp_* symbols, so exactly one must be linked per build. The active BSP is
# chosen in src/idf_component.yml via the FREEBUFF_BOARD environment variable
# (set below, before project() runs the component manager).
option(WAVESHARE_AMOLED_2_06_BOARD "Build for Waveshare ESP32-S3 Touch AMOLED 2.06" ON)
option(WAVESHARE_AMOLED_1_8_BOARD "Build for Waveshare ESP32-S3 Touch AMOLED 1.8" OFF)
option(AIPI_LITE_BOARD "Build for AIPI-Lite board" OFF)
if(WAVESHARE_AMOLED_2_06_BOARD)
  add_compile_definitions(WAVESHARE_AMOLED_2_06_BOARD=1)
  add_compile_definitions(WAVESHARE_AMOLED_1_8_BOARD=0)
  add_compile_definitions(AIPI_LITE_BOARD=0)
  add_compile_definitions(WAVESHARE_BSP_BOARD=1)
  set(ENV{FREEBUFF_BOARD} "waveshare_amoled_2_06")
elseif(WAVESHARE_AMOLED_1_8_BOARD)
  add_compile_definitions(WAVESHARE_AMOLED_2_06_BOARD=0)
  add_compile_definitions(WAVESHARE_AMOLED_1_8_BOARD=1)
  add_compile_definitions(AIPI_LITE_BOARD=0)
  add_compile_definitions(WAVESHARE_BSP_BOARD=1)
  set(ENV{FREEBUFF_BOARD} "waveshare_amoled_1_8")
elseif(AIPI_LITE_BOARD)
  add_compile_definitions(WAVESHARE_AMOLED_2_06_BOARD=0)
  add_compile_definitions(WAVESHARE_AMOLED_1_8_BOARD=0)
  add_compile_definitions(AIPI_LITE_BOARD=1)
  set(ENV{FREEBUFF_BOARD} "waveshare_amoled_1_8")
else()
  add_compile_definitions(WAVESHARE_AMOLED_2_06_BOARD=0)
  add_compile_definitions(WAVESHARE_AMOLED_1_8_BOARD=0)
  add_compile_definitions(AIPI_LITE_BOARD=0)
  set(ENV{FREEBUFF_BOARD} "waveshare_amoled_1_8")
endif()

add_compile_definitions(OPENAI_REALTIMEAPI="https://speech.fleming.ai/v1/realtime")

# TURN relay server for WebRTC/ICE media. The URL is public; the username and
# password (coturn long-term credentials) come from the gitignored
# privateConfig.json when present. Without credentials the client skips the
# TURN allocation and falls back to host/STUN candidates only.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/privateConfig.json")
  file(READ privateConfig.json PV_CFG)
  string(JSON PV_TURN_USERNAME ERROR_VARIABLE PV_TURN_USERNAME_ERR GET "${PV_CFG}" turn_username)
  if(PV_TURN_USERNAME_ERR)
    set(PV_TURN_USERNAME "")
    set(PV_TURN_PASSWORD "")
    set(PV_TURN_SERVER "")
  else()
    string(JSON PV_TURN_PASSWORD GET "${PV_CFG}" turn_password)
    string(JSON PV_TURN_SERVER ERROR_VARIABLE PV_TURN_SERVER_ERR GET "${PV_CFG}" turn_server)
    if(PV_TURN_SERVER_ERR)
      set(PV_TURN_SERVER "turn:turn.fleming.ai:3478")
    endif()
  endif()
else()
  set(PV_TURN_USERNAME "")
  set(PV_TURN_PASSWORD "")
  set(PV_TURN_SERVER "")
endif()
if(PV_TURN_SERVER)
  add_compile_definitions(TURN_SERVER_URL="${PV_TURN_SERVER}")
endif()
add_compile_definitions(TURN_USERNAME="${PV_TURN_USERNAME}")
add_compile_definitions(TURN_PASSWORD="${PV_TURN_PASSWORD}")

# Waveshare board-specific sdkconfig overlay
if(WAVESHARE_AMOLED_2_06_BOARD)
  set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.waveshare_amoled_2_06")
elseif(WAVESHARE_AMOLED_1_8_BOARD)
  set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.waveshare_amoled_1_8")
endif()

set(COMPONENTS src)
set(EXTRA_COMPONENT_DIRS "src" "components/srtp" "components/peer" "components/esp-libopus")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(src)
