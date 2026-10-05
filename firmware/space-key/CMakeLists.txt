cmake_minimum_required(VERSION 3.16)

# 和 voice-bot / usage-monitor 一样:上游 ai-passport 以 submodule 形式挂在
# upstream/ai-passport,这里用 EXTRA_COMPONENT_DIRS 引入它的 BSP,不改上游一行。
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../upstream/ai-passport/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(space-key)
