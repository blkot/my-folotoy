cmake_minimum_required(VERSION 3.16)

# 与 voice-bot 同样的做法:上游 ai-passport 以 submodule 形式放在
# upstream/ai-passport,这里把它当额外组件目录引入,直接用它的 BSP。
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../upstream/ai-passport/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(visual-novel)
