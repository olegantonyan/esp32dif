idf_component_register(SRCS "web.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES settings net ota rtp digi esp_http_server esp_wifi esp_netif esp_timer esp_app_format esp_system mbedtls lwip freertos
                       EMBED_TXTFILES "index.html")
