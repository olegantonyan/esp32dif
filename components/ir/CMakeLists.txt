idf_component_register(SRCS "ir.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_driver_gpio esp_timer freertos)
