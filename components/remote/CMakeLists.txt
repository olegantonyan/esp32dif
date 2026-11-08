idf_component_register(SRCS "remote.c" "keymap.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common ir
                       PRIV_REQUIRES settings rtp lwip esp_timer freertos)
