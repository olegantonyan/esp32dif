idf_component_register(SRCS "cmd_player.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES remote lwip shell console)
