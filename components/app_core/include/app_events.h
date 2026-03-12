#pragma once

#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

// 声明应用全局事件基础
ESP_EVENT_DECLARE_BASE(APP_EVENT_BASE);

// 定义事件ID
typedef enum {
    APP_EVENT_SMS_RECEIVED,    // 收到短信事件
} app_event_id_t;

// 短信接收事件的数据结构
typedef struct {
    char sender[32];
    char text[2048];
    char timestamp[32];
} app_event_sms_received_data_t;

#ifdef __cplusplus
}
#endif
