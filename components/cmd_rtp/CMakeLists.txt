idf_component_register(SRCS "cmd_rtp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES rtp net settings shell console esp_timer lwip)
