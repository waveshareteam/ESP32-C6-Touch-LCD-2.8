cmake_minimum_required(VERSION 3.16)
# Build only components reachable from the factory APP.
set(COMPONENTS main)
set(PROJECT_VER "V1.0")
set(IDF_TARGET esp32c6)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(01_factory)

# LVGL's TJpgDec structure differs from the ESP32-C6 ROM decoder ABI.
set(tjpgd_symbols jd_prepare=lvgl_jd_prepare jd_decomp=lvgl_jd_decomp)
foreach(component lvgl__lvgl Photos)
    idf_component_get_property(tjpgd_lib ${component} COMPONENT_LIB)
    target_compile_definitions(${tjpgd_lib} PRIVATE ${tjpgd_symbols})
endforeach()
