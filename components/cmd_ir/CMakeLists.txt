idf_component_register(SRCS "cmd_ir.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES ir remote lwip shell console esp_timer)
