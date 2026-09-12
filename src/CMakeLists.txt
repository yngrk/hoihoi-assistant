idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES port_bsp driver esp_adc esp_timer esp_psram)
