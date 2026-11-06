idf_component_register(SRCS "cmd_led.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES led shell console)
