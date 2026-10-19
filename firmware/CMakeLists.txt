cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# TinyUSB mass storage for file mode (main/file_mode.cpp); the buffer is one transfer to or from the card
add_compile_definitions(CFG_TUD_MSC=1 CFG_TUD_MSC_EP_BUFSIZE=4096)

project(myplaud_usb_audio_test)
