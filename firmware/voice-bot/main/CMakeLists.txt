idf_component_register(
    SRCS "main.c"
         "voice_audio.c"
         "voice_net.c"
         "voice_ui.c"
         "voice_app.c"
         "voice_prov.c"
         "voice_prov_radio.c"
         "voice_prov_security.c"
    INCLUDE_DIRS "."
    # mbedtls 是配网安全协商(AES/DHM/MD5)需要的;bt 提供 NimBLE 与 BLUFI。
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi lwip mbedtls nvs_flash
)

# 中文字库放在仓库顶层的 shared/fonts,两个固件共用一份(3.5 MB,不重复存)。
# 来源:Noto Sans SC (OFL-1.1) 经 lv_font_conv 生成的 GB2312 子集。
# 识别结果和回复是中文,默认的 Montserrat 没有汉字。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../../shared/fonts/vn_font_16.c"
)
