idf_component_register(
    SRCS "main.c"
         "vn_engine.c"
         "vn_client.c"
         "vn_bg.c"
         "vn_ui.c"
         "vn_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_hw_support esp_http_client esp_lcd esp_netif esp_partition esp_timer esp_wifi lwip nvs_flash
)

# 中文字库放在仓库顶层的 shared/fonts,两个固件共用一份(3.5 MB,不重复存)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../../shared/fonts/vn_font_16.c"
)
