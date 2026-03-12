#include <stdio.h>
#include <string.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_event.h"

#include "app_events.h"
#include "config.h"
#include "modem.h"
#include "sms.h"
#include "wifi_mgr.h"
#include "web_server.h"
#include "push.h"
#include "utils.h"

static const char *TAG = "main";
static const char *SCHED_TAG = "sched";

// 固件构建信息：包含编译日期和时间，用于确认当前运行的固件版本
const char *g_fw_build = __DATE__ " " __TIME__;

// 短信通知队列，存储堆分配的 app_event_sms_received_data_t 指针
// 队列深度为4，防止短时间内多条短信排队时丢失
static QueueHandle_t s_sms_queue;

// 模组初始化：AT握手 + 基本配置
// 禁用数据连接（省流量）：先查 CGACT 状态，仅在实际激活时才发 CGACT=0,1
static void modem_disable_data_context(void)
{
    if (modem_cgact_is_active(1)) {
        ESP_LOGI(TAG, "当前 CGACT: context 1 已激活，尝试关闭...");
        bool cgact_ok = false;
        for (int i = 0; i < 5; i++) {
            if (modem_cgact_deactivate(1)) {
                cgact_ok = true;
                break;
            }
            ESP_LOGW(TAG, "CGACT设置失败，重试 %d/5...", i + 1);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (cgact_ok) {
            ESP_LOGI(TAG, "已禁用数据连接");
        } else {
            ESP_LOGW(TAG, "CGACT 未返回 OK，继续启动");
        }
    } else {
        ESP_LOGI(TAG, "当前 CGACT: context 1 未激活，无需发送 CGACT=0,1");
    }
}

static void modem_setup(void)
{
    // AT握手
    while (!modem_send_at_wait_ok("AT", 1000)) {
        ESP_LOGW(TAG, "AT未响应，重试...");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "模组AT响应正常");

    // 设置短信自动上报：2,1,0,0,0 确保新短信先存 SIM 再上报（部分模组用 +CMTI 通知）
    while (!modem_send_at_wait_ok("AT+CNMI=2,1,0,0,0", 1000)) {
        ESP_LOGW(TAG, "CNMI设置失败，重试...");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "CNMI设置完成");

    // PDU模式
    while (!modem_send_at_wait_ok("AT+CMGF=0", 1000)) {
        ESP_LOGW(TAG, "PDU模式设置失败，重试...");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "PDU模式设置完成");

    // 新短信存储位置：设为 SIM，这样 +CMTI 报 "SM",index 后可用 AT+CMGR=index 读出内容并上报/入环缓冲（与 Web 历史来自内存缓冲无关）
    if (modem_send_at_wait_ok("AT+CPMS=\"SM\",\"SM\",\"SM\"", 2000)) {
        ESP_LOGI(TAG, "新短信存储已设为 SIM（便于按 CMTI 索引读取）");
    } else {
        ESP_LOGW(TAG, "CPMS 设置失败，使用模组默认存储");
    }

    // 网络注册前先尝试关闭一次数据连接（部分模组上电后可能默认已激活 PDP）
    modem_disable_data_context();

    // 等待网络注册
    while (!modem_wait_cereg()) {
        ESP_LOGW(TAG, "等待网络注册...");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "网络已注册");

    // 网络注册完成后再次检查，如果 PDP 仍被激活则再关一次，确保最终关闭
    modem_disable_data_context();
}

// 短信通知任务：运行在独立的大栈任务中，执行推送/发邮件/回复短信等重量级操作
// sys_evt 任务栈仅 ~2800 字节，不足以承载 TLS/HTTP/UART 调用，必须分离
static void sms_notify_task(void *arg)
{
    app_event_sms_received_data_t *data;
    while (1) {
        if (xQueueReceive(s_sms_queue, &data, portMAX_DELAY) != pdTRUE) continue;

        ESP_LOGI(TAG, "sms_notify_task: 从队列取出短信事件，开始分发推送 sender=%s", data->sender);

        // 检查是否是 WiFi 配置指令
        if (strncmp(data->text, "WIFI:", 5) == 0) {
            ESP_LOGI(TAG, "检测到 WiFi 配置指令");
            char ssid[33] = {0};
            char pass[65] = {0};
            const char *comma = strchr(data->text + 5, ',');
            if (comma) {
                int ssid_len = comma - (data->text + 5);
                if (ssid_len > 32) ssid_len = 32;
                strncpy(ssid, data->text + 5, ssid_len);
                ssid[ssid_len] = '\0';
                strncpy(pass, comma + 1, sizeof(pass) - 1);
                pass[sizeof(pass) - 1] = '\0';

                char *newline = strchr(pass, '\r');
                if (newline) *newline = '\0';
                newline = strchr(pass, '\n');
                if (newline) *newline = '\0';

                ESP_LOGI(TAG, "提取到 SSID: %s, PASS: %s", ssid, pass);
                config_set_wifi_sta(ssid, pass);

                char reply[128];
                snprintf(reply, sizeof(reply), "WiFi配置已更新: %s, 正在重启网络", ssid);
                sms_send(data->sender, reply);

                free(data);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
                return;
            }
        }

        // 推送到所有启用的通道（含 SMTP 邮件通道）
        push_send_to_all(data->sender, data->text, data->timestamp);
        free(data);
    }
}

// 短信轮询任务：独立 task 中轮询 URC 与长短信超时，不占用 app_main
static void sms_poll_task(void *arg)
{
    while (1) {
        sms_check_urc();
        sms_check_concat_timeout();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// 定时任务：检查是否到点并执行（短信/PING），结果推送并写回 NVS
static void scheduler_task(void *arg)
{
    char msg_buf[MAX_LAST_MSG_LEN];
    char ts_buf[32];

    ESP_LOGI(SCHED_TAG, "定时任务调度器已启动（按分钟触发，时区由 WiFi/NTP 模块设置）");
    for (int i = 0; i < MAX_SCHEDULE_TASKS; i++) {
        const schedule_task_t *t = &g_schedule_tasks[i];
        if (!t->enabled) continue;
        ESP_LOGI(SCHED_TAG,
                 "任务#%d 已启用: action=%d kind=%d time=%02d:%02d wdayMask=0x%02x monInt=%d initialTs=%lld phone=%s host=%s",
                 i + 1, (int)t->action, (int)t->kind, (int)t->hour, (int)t->minute,
                 (int)t->weekday_mask, (int)t->month_interval, (long long)t->initial_ts,
                 t->phone, t->ping_host);
    }

    while (1) {
        time_t now = time(NULL);
        if (now < 100000) {
            // 时间未同步时每 5 秒检查一次，避免错过 NTP 生效后的第一个触发点
            static uint32_t last_warn_tick = 0;
            uint32_t tick = xTaskGetTickCount();
            if (tick - last_warn_tick > pdMS_TO_TICKS(30000)) {
                ESP_LOGW(SCHED_TAG, "系统时间未同步（time=%lld），暂不触发定时任务", (long long)now);
                last_warn_tick = tick;
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // 对齐到“下一分钟边界”，避免 vTaskDelay(60000) 漂移导致偶发错过触发窗口
        struct tm tm_now;
        if (!localtime_r(&now, &tm_now)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int sleep_sec = 60 - (tm_now.tm_sec % 60);
        if (sleep_sec <= 0 || sleep_sec > 60) sleep_sec = 60;
        vTaskDelay(pdMS_TO_TICKS((uint32_t)sleep_sec * 1000));

        now = time(NULL);
        if (!localtime_r(&now, &tm_now)) continue;

        bool sched_dirty = false;
        for (int i = 0; i < MAX_SCHEDULE_TASKS; i++) {
            schedule_task_t *t = &g_schedule_tasks[i];
            if (!t->enabled) continue;

            int run_hour = (int)t->hour;
            int run_minute = (int)t->minute;
            if (t->kind == SCHED_KIND_MONTH && t->initial_ts != 0) {
                struct tm ref;
                if (localtime_r(&t->initial_ts, &ref)) {
                    run_hour = ref.tm_hour;
                    run_minute = ref.tm_min;
                }
            }
            if (tm_now.tm_hour != run_hour || tm_now.tm_min != run_minute) continue;

            bool run_today = false;
            if (t->kind == SCHED_KIND_DAY) {
                run_today = true;
            } else if (t->kind == SCHED_KIND_WEEK) {
                if (t->weekday_mask & (1u << (tm_now.tm_wday & 7))) run_today = true;
            } else if (t->kind == SCHED_KIND_MONTH && t->initial_ts != 0) {
                struct tm ref;
                if (!localtime_r(&t->initial_ts, &ref)) continue;
                if (tm_now.tm_mday != ref.tm_mday) continue;
                int cur_month = tm_now.tm_mon + 1;
                int init_month = ref.tm_mon + 1;
                if (cur_month < init_month) continue;
                if (t->month_interval > 0 && ((cur_month - init_month) % t->month_interval) != 0) continue;
                run_today = true;
            }

            if (!run_today) continue;

            struct tm tm_today = {0};
            tm_today.tm_year = tm_now.tm_year;
            tm_today.tm_mon  = tm_now.tm_mon;
            tm_today.tm_mday = tm_now.tm_mday;
            tm_today.tm_hour = run_hour;
            tm_today.tm_min  = run_minute;
            tm_today.tm_sec  = 0;
            time_t trigger_ts = mktime(&tm_today);
            if (trigger_ts == (time_t)-1) continue;
            if (t->last_run_ts >= trigger_ts && (t->last_run_ts - trigger_ts) < 120) continue;  // 2 分钟内已执行过

            // 执行任务
            bool ok = false;
            msg_buf[0] = '\0';
            if (t->action == SCHED_ACTION_SMS) {
                ok = sms_send(t->phone, t->message[0] ? t->message : "定时短信");
                snprintf(msg_buf, sizeof(msg_buf), "%s", ok ? "短信发送成功" : "短信发送失败");
            } else {
                const char *host = t->ping_host[0] ? t->ping_host : "8.8.8.8";
                ok = modem_ping(host, msg_buf, sizeof(msg_buf));
                if (!msg_buf[0]) snprintf(msg_buf, sizeof(msg_buf), "%s", ok ? "Ping 成功" : "Ping 失败");
            }

            t->last_run_ts = now;
            t->last_ok = ok;
            safe_strcpy(t->last_msg, msg_buf, sizeof(t->last_msg));
            sched_dirty = true;

            // 推送执行结果（走 sms_notify_task 大栈任务，避免在 scheduler 小栈里跑 TLS/HTTP 导致溢出）
            strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S", &tm_now);
            char body[128];
            snprintf(body, sizeof(body), "定时任务#%d %s\n%s\n%s", i + 1, ok ? "成功" : "失败", msg_buf, ts_buf);
            app_event_sms_received_data_t *evt = malloc(sizeof(app_event_sms_received_data_t));
            if (evt) {
                snprintf(evt->sender, sizeof(evt->sender), "%s", "定时任务");
                snprintf(evt->text, sizeof(evt->text), "%s", body);
                snprintf(evt->timestamp, sizeof(evt->timestamp), "%s", ts_buf);
                if (xQueueSend(s_sms_queue, &evt, pdMS_TO_TICKS(1000)) != pdTRUE) {
                    ESP_LOGW(SCHED_TAG, "通知队列已满，丢弃定时任务推送");
                    free(evt);
                }
            } else {
                ESP_LOGW(SCHED_TAG, "内存不足，无法提交定时任务推送（仅更新 lastRun/lastMsg）");
            }
            ESP_LOGI(SCHED_TAG, "定时任务#%d 执行完毕: %s（%s）", i + 1, ok ? "成功" : "失败", msg_buf);
        }
        if (sched_dirty) schedule_save();
    }
}

// 事件处理器：仅将数据入队，不做任何耗时操作，避免 sys_evt 任务栈溢出
static void sms_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    if (event_base == APP_EVENT_BASE && event_id == APP_EVENT_SMS_RECEIVED) {
        ESP_LOGI(TAG, "收到 APP_EVENT_SMS_RECEIVED，准备入队");
        app_event_sms_received_data_t *data = malloc(sizeof(app_event_sms_received_data_t));
        if (!data) {
            ESP_LOGE(TAG, "内存不足，无法处理短信事件");
            return;
        }
        memcpy(data, event_data, sizeof(app_event_sms_received_data_t));
        if (xQueueSend(s_sms_queue, &data, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "短信通知队列已满，丢弃本条消息");
            free(data);
        } else {
            ESP_LOGI(TAG, "短信事件入队成功，sender=%.32s", data->sender);
        }
    }
}

void app_main(void)
{
    // 初始化默认事件循环
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    // 创建短信通知队列（深度4，存指针）
    s_sms_queue = xQueueCreate(4, sizeof(app_event_sms_received_data_t *));
    ESP_ERROR_CHECK(s_sms_queue ? ESP_OK : ESP_ERR_NO_MEM);

    // 创建短信通知任务，分配 12KB 栈以容纳 TLS/HTTP/SMTP/PDU 编码调用链
    xTaskCreate(sms_notify_task, "sms_notify", 12288, NULL, 5, NULL);

    // 注册短信接收事件监听器（轻量入队，不做重操作）
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        APP_EVENT_BASE, APP_EVENT_SMS_RECEIVED,
        &sms_event_handler, NULL, NULL));

    // 日志分级：modem/sms 设为 DEBUG 便于排查 AT/URC；其余模块使用默认（INFO）
    // ERROR=失败/异常，WARN=可恢复，INFO=关键状态，DEBUG=AT/PDU/解析等
    esp_log_level_set("modem", ESP_LOG_DEBUG);
    esp_log_level_set("sms",   ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "====== 短信转发器启动 ======");
    ESP_LOGI(TAG, "固件构建时间: %s", g_fw_build);

    // 初始化模组UART
    modem_init();

    // 模组从干净状态启动
    modem_flush_rx();
    modem_power_cycle();
    modem_flush_rx();

    // 初始化短信处理模块
    sms_init();

    // 加载配置（NVS）
    config_init();

    // 模组配置
    modem_setup();

    // 在 UART/模组就绪后再创建依赖 modem 的任务，避免 uart_read_bytes 在驱动未初始化时被调用
    xTaskCreate(sms_poll_task, "sms_poll", 4096, NULL, 4, NULL);
    // scheduler 内部可能调用 sms_send/modem_ping，且会提交推送到 sms_notify（大栈）执行
    xTaskCreate(scheduler_task, "scheduler", 12288, NULL, 3, NULL);

    // WiFi连接
    wifi_mgr_init();

    // NTP时间同步
    wifi_mgr_sync_ntp();

    // 启动Web服务器
    web_server_start();

    // 启动通知：通过推送通道统一发送（含 SMTP）
    if (g_config_valid) {
        ESP_LOGI(TAG, "推送通道配置有效，发送启动通知...");
        char url[64];
        wifi_mgr_get_device_url(url, sizeof(url));
        char body[128];
        snprintf(body, sizeof(body), "设备已启动\n设备地址: %s", url);
        push_send_to_all("系统", body, "");
    }

    ESP_LOGI(TAG, "====== 初始化完成，各任务已就绪 ======");

    // 配置无效时提示一次
    if (!g_config_valid) {
        char url[64];
        wifi_mgr_get_device_url(url, sizeof(url));
        ESP_LOGW(TAG, "推送通道未配置，请访问 %s 进行配置", url);
    }

    // app_main 仅做初始化，短信轮询由 sms_poll_task 承担
}