idf_component_register(SRCS "main.cpp" "dark/scr_bg.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_c6_touch_lcd_2_8 brookesia_core Settings DrawingBoard Weather
             Music Sensor Games XiaozhiApp AIChats MicTest ButtonTest DisplayTest
             Qmi8658Ball network_service esp_wifi bsp_extra)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
