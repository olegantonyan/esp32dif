idf_component_register(SRCS "cmd_digi.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES digi shell console freertos)
