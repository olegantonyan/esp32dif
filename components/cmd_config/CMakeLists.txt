idf_component_register(SRCS "cmd_config.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES settings net shell console)
