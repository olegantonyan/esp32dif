idf_component_register(SRCS "shell.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common console
                       PRIV_REQUIRES vfs esp_stdio freertos)
