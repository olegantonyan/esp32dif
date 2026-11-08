idf_component_register(SRCS "telnet.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES shell settings console lwip esp_timer freertos)
