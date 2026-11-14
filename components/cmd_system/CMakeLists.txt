idf_component_register(SRCS "cmd_system.c" "cmd_sys.c" "cmd_mem.c" "cmd_i2c.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES shell console i2c_bus spi_flash esp_app_format esp_timer esp_hw_support heap freertos)
