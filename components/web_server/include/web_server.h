#pragma once

#include "esp_http_server.h"

// 启动HTTP服务器
httpd_handle_t web_server_start(void);

// 停止HTTP服务器
void web_server_stop(httpd_handle_t server);
