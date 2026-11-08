idf_component_register(SRCS "ota.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES app_update esp_app_format bootloader_support esp_partition esp_timer esp_system log)
