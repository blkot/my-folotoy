idf_component_register(
    SRCS "main.c"
         "space_key.c"
         "space_ui.c"
         "esp_hid_gap.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_hid
    PRIV_REQUIRES nvs_flash
)

# 中文字库放在仓库顶层的 shared/fonts,各固件共用一份。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../../shared/fonts/vn_font_16.c"
)
