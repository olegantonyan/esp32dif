idf_component_register(SRCS "cmd_ota.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES ota shell console freertos)
