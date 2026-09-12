idf_component_register(
    SRCS "main.cpp" "gfx.cpp" "audio.cpp" "font5x7.cpp"
    INCLUDE_DIRS "."
    REQUIRES port_bsp driver esp_adc esp_timer esp_psram esp_codec_dev)
