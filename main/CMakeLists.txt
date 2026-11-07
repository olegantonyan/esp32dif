idf_component_register(SRCS "main.c" "status_led.c" "status_oled.c"
                       PRIV_REQUIRES led oled shell cmd_system cmd_led cmd_oled digi cmd_digi esp_timer heap)
