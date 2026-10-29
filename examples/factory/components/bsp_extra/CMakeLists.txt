idf_component_register(SRCS "src/bsp_board_extra.cpp"
    INCLUDE_DIRS "include" REQUIRES esp32_c6_touch_lcd_2_8 spiffs
    PRIV_REQUIRES esp_timer)
