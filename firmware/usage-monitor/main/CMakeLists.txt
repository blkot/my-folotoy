idf_component_register(
    SRCS "main.c"
         "usage_model.c"
         "usage_net.c"
         "usage_ui.c"
         "usage_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_http_client esp_hw_support esp_netif esp_timer esp_wifi lwip nvs_flash
)

# 中文字库放在仓库顶层的 shared/fonts,各固件共用一份(3.5 MB,不重复存)。
# 平台名、窗口标签、明细文本都可能是中文。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../../shared/fonts/vn_font_16.c"
)
