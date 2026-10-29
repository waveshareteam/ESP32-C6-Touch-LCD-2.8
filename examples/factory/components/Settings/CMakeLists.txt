idf_component_register(
    SRCS "Settings.cpp" "service/settings_service.cpp" "assets/img_app_settings.c"
    INCLUDE_DIRS "." "service"
    REQUIRES bsp_extra brookesia_core lvgl esp32_c6_touch_lcd_2_8 network_service
    PRIV_REQUIRES nvs_flash esp_wifi esp_app_format esp_system esp_hw_support spi_flash)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
