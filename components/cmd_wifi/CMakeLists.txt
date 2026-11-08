idf_component_register(SRCS "cmd_wifi.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES net settings shell console esp_wifi esp_netif esp_timer)
