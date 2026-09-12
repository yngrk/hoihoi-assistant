idf_component_register(
    SRCS "main.cpp" "gfx.cpp" "audio.cpp" "font5x7.cpp" "display_sync.cpp"
         "listen.cpp" "logview.cpp" "net.cpp" "verbindung.cpp" "stt.cpp"
         "chat.cpp" "tts.cpp" "cfg.cpp" "prov.cpp" "nachtrag.cpp"
         "wachwort.cpp" "merkmal.cpp" "vergleich.cpp"
    INCLUDE_DIRS "."
    REQUIRES port_bsp driver esp_adc esp_timer esp_psram esp_codec_dev esp-dsp
             esp_wifi esp_netif nvs_flash esp_event esp_websocket_client esp_http_client
             wifi_provisioning bt
             json esp-tls mbedtls)
