# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-sms-forwarder)

# 版本信息：v1.0.1-dirty Build202603131149，dirty 仅在有未提交变更时出现
set(VERSION_INFO_H "${CMAKE_BINARY_DIR}/include/version_info.h")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/include")
execute_process(
  COMMAND git describe --tags --always --dirty
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE GIT_DESCRIBE
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT GIT_DESCRIBE)
  set(GIT_DESCRIBE "v0.0.0")
endif()
execute_process(
  COMMAND date +%Y%m%d%H%M
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE BUILD_TIME
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT BUILD_TIME)
  set(BUILD_TIME "000000000000")
endif()
string(REGEX REPLACE "-dirty$" "" GIT_VER_CLEAN "${GIT_DESCRIBE}")
string(REGEX REPLACE "^v" "" GIT_VER_CLEAN "${GIT_VER_CLEAN}")
string(FIND "${GIT_DESCRIBE}" "-dirty" DIRTY_IDX)
if(DIRTY_IDX GREATER_EQUAL 0)
  set(APP_DIRTY_SUFFIX "-dirty")
else()
  set(APP_DIRTY_SUFFIX "")
endif()
set(APP_FW_VERSION_STR "${GIT_VER_CLEAN}${APP_DIRTY_SUFFIX} Build${BUILD_TIME}")
file(WRITE "${VERSION_INFO_H}"
  "// 自动生成，请勿编辑\n#ifndef VERSION_INFO_H\n#define VERSION_INFO_H\n#define APP_FW_VERSION_STR \"${APP_FW_VERSION_STR}\"\n#endif\n")
target_include_directories(__idf_main PRIVATE "${CMAKE_BINARY_DIR}/include")

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

# 打包两种固件：factory 仅放合并后的整片固件，upgrade 仅放 OTA 用 app
# 产出：build/release/factory/esp32-sms-forwarder_factory.bin、build/release/upgrade/esp32-sms-forwarder_upgrade.bin
set(RELEASE_DIR "${CMAKE_BINARY_DIR}/release")
set(FACTORY_DIR "${RELEASE_DIR}/factory")
set(UPGRADE_DIR "${RELEASE_DIR}/upgrade")
set(APP_BIN "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin")
set(FACTORY_BIN_NAME "${PROJECT_NAME}_factory.bin")
set(UPGRADE_BIN_NAME "${PROJECT_NAME}_upgrade.bin")
set(MERGE_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/merge_factory_bin.py")
set(IDF_PATH_FOR_MERGE "$ENV{IDF_PATH}")

add_custom_target(package
  ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${FACTORY_DIR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${UPGRADE_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${APP_BIN}" "${UPGRADE_DIR}/${UPGRADE_BIN_NAME}"
  COMMENT "打包 factory（合并固件）与 upgrade（OTA 固件）到 build/release/"
)
# factory：仅合并后的单文件（与 idf.py flash 写入内容一致）
# 刷写示例: esptool.py -p <串口> write_flash 0x0 build/release/factory/esp32-sms-forwarder_factory.bin
find_package(Python3 REQUIRED)
add_custom_command(TARGET package
  COMMAND ${Python3_EXECUTABLE} "${MERGE_SCRIPT}" "${CMAKE_BINARY_DIR}" "${FACTORY_DIR}/${FACTORY_BIN_NAME}" "${IDF_PATH_FOR_MERGE}"
  COMMENT "合并 factory 固件为 ${FACTORY_BIN_NAME}"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
# 依赖 gen_project_binary（产出 app 的 .bin）及 NVS，确保复制前各 bin 已生成
add_dependencies(package gen_project_binary)
if(NOT SKIP_NVS_FLASH)
  add_dependencies(package nvs_nvs_bin)
endif()

