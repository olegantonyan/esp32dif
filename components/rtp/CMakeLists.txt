idf_component_register(SRCS "rtp.c" "rtp_play.c" "rtp_sap.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES digi lwip esp_timer freertos log heap)
