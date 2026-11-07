idf_component_register(SRCS "digi.c" "wm8804.c" "spdif_rx.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common esp_driver_gpio
                       PRIV_REQUIRES i2c_bus esp_driver_i2c esp_driver_i2s esp_driver_pcnt esp_driver_rmt heap esp_timer esp_rom freertos)
