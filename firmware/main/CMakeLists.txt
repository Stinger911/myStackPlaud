idf_component_register(
    SRCS "main.cpp" "screen.cpp" "audio.cpp" "recorder.cpp" "keyboard.cpp" "settings.cpp" "file_mode.cpp"
    INCLUDE_DIRS "."
)

# File mode: the audio USB descriptors are wrapped (file_mode.cpp) so that one image can present either
# a headset or a flash drive.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=tud_descriptor_device_cb"
    "-Wl,--wrap=tud_descriptor_configuration_cb")
