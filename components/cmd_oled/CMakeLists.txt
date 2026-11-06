idf_component_register(SRCS "cmd_oled.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES oled shell console)
