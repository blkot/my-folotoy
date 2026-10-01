cmake_minimum_required(VERSION 3.16)

# 上游 ai-passport 以 submodule 形式放在 upstream/ai-passport,保持原样不动。
# 这里把它当作"额外的组件目录"引入,于是我们能用它的 BSP(components/bsp),
# 却不需要复制它的代码,也不用在它上面做任何修改。
#
# 同理,shared/ 存放两个固件共用的资源(目前是 3.5 MB 的 GB2312 中文字库),
# 只保留一份,避免每个固件各复制一遍。
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../upstream/ai-passport/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(voice-bot)
