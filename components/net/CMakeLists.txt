idf_component_register(SRCS "net.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif esp_wifi
                       PRIV_REQUIRES esp_event esp_hw_support esp_timer nvs_flash)
