set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

set(PROJ_SRCS_C
    "../AIChats/assets/img_app_aichats.c"
    "XiaozhiTlsBuffer.c"
)

set(PROJ_SRCS_CPP
    "XiaozhiActivationClient.cpp"
    "XiaozhiActivationPrompt.cpp"
    "XiaozhiApp.cpp"
    "XiaozhiAudioProcessor.cpp"
    "XiaozhiClient.cpp"
    "XiaozhiLocalization.cpp"
    "XiaozhiUi.cpp"
)

set(PROJ_SRCS_CC
    "third_party/xiaozhi_esp32/ogg_demuxer.cc"
)

idf_component_register(
    SRCS ${PROJ_SRCS_C} ${PROJ_SRCS_CPP} ${PROJ_SRCS_CC}
    INCLUDE_DIRS ${PROJ_SRC}
    REQUIRES bsp_extra brookesia_core esp32_c6_touch_lcd_2_8 esp_audio_codec esp_audio_effects esp-sr xiaozhi-fonts esp_event esp_netif esp_xiaozhi nvs_flash network_service
    PRIV_REQUIRES
        app_update
        esp_http_client
        cjson
        efuse
        esp_hw_support
        esp_app_format
        esp_partition
        esp_timer
        mbedtls
        mcp-c-sdk
        spi_flash
)

if(CONFIG_MBEDTLS_DYNAMIC_BUFFER)
    idf_component_get_property(mbedtls_dir mbedtls COMPONENT_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${mbedtls_dir}/mbedtls/library"
        "${mbedtls_dir}/port/dynamic"
    )
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=esp_mbedtls_free_tx_buffer"
        "-Wl,--undefined=__wrap_esp_mbedtls_free_tx_buffer"
    )
endif()

set_source_files_properties(
    ${PROJ_SRCS_CPP} ${PROJ_SRCS_CC}
    PROPERTIES
        COMPILE_FLAGS "-Wno-missing-field-initializers"
)
