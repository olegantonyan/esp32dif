idf_component_register(SRCS "main.c" "status_led.c" "status_oled.c"
                       PRIV_REQUIRES settings ota cmd_ota cmd_config web telnet led oled shell cmd_system cmd_led cmd_oled digi cmd_digi net cmd_wifi rtp cmd_rtp esp_netif esp_timer heap)
