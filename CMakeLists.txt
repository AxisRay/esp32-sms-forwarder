# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-sms-forwarder)

# 自动构建 Vue 前端：idf.py build 时先执行 npm run build，产物供 web_server 嵌入
set(WEB_DIR "${CMAKE_SOURCE_DIR}/web")
set(WEB_INDEX_GZ "${WEB_DIR}/dist-web/index.html.gz")
file(GLOB_RECURSE WEB_SOURCES "${WEB_DIR}/src/*" "${WEB_DIR}/index.html")
add_custom_command(
    OUTPUT "${WEB_INDEX_GZ}"
    COMMAND npm run build
    WORKING_DIRECTORY "${WEB_DIR}"
    DEPENDS "${WEB_DIR}/package.json" "${WEB_DIR}/vite.config.js" ${WEB_SOURCES}
    COMMENT "构建前端 Vue 应用 (npm run build)"
)
add_custom_target(web_frontend ALL DEPENDS "${WEB_INDEX_GZ}")
add_dependencies(__idf_web_server web_frontend)

# 是否在刷机时烧录 NVS 分区（nvs_config.csv）。设为 OFF 可只更新固件、保留设备内已有配置。
# 用法：idf.py -DSKIP_NVS_FLASH=ON flash  或先 cmake -DSKIP_NVS_FLASH=ON . 再 idf.py flash
set(SKIP_NVS_FLASH OFF CACHE BOOL "若 ON，则不生成/烧录 NVS 分区，保留设备原有配置")
if(NOT SKIP_NVS_FLASH)
  # 使用CSV文件生成NVS分区镜像，配置 FLASH_IN_PROJECT 会在 idf.py flash 时自动烧录
  nvs_create_partition_image(nvs nvs_config.csv FLASH_IN_PROJECT)
endif()

