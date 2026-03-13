# 嵌入的前端文件由项目根 CMakeLists.txt 中的 web_frontend 目标生成
set(WEB_INDEX_GZ "${CMAKE_SOURCE_DIR}/web/dist-web/index.html.gz")

idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json app_core network_manager modem_driver notification app_update
    EMBED_FILES "${WEB_INDEX_GZ}"
)
