#include "web_server.h"
#include "config.h"
#include <modem.h>
#include <sms.h>
#include <smtp_client.h>
#include <push.h>
#include <wifi_mgr.h>
#include <utils.h>
#include <app_events.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <freertos/FreeRTOS.h>
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdarg.h>

static const char *TAG = "web_srv";

// 来自 main.c 的全局固件构建信息（编译时间戳）
extern const char *g_fw_build;

// 声明嵌入的前端文件
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

/* ========== Basic Auth ========== */
static bool check_auth(httpd_req_t *req)
{
    char auth_buf[256] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_buf, sizeof(auth_buf)) != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"SMS Forwarding\"");
        httpd_resp_send(req, "Unauthorized", -1);
        return false;
    }
    // 解码 "Basic base64(user:pass)"
    if (strncmp(auth_buf, "Basic ", 6) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"SMS Forwarding\"");
        httpd_resp_send(req, "Unauthorized", -1);
        return false;
    }
    const char *b64 = auth_buf + 6;
    size_t decoded_len = 0;
    unsigned char decoded[256] = {0};
    extern int mbedtls_base64_decode(unsigned char *, size_t, size_t *, const unsigned char *, size_t);
    mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len, (const unsigned char *)b64, strlen(b64));
    decoded[decoded_len] = '\0';

    char expected[192];
    snprintf(expected, sizeof(expected), "%s:%s", g_config.web_user, g_config.web_pass);
    if (strcmp((const char *)decoded, expected) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"SMS Forwarding\"");
        httpd_resp_send(req, "Unauthorized", -1);
        return false;
    }
    return true;
}

/* ========== 公共 JSON 请求/响应助手 ========== */

// 读取请求体并解析为 cJSON，失败时自动发送错误响应并返回 NULL
static cJSON *read_json_body(httpd_req_t *req, int max_len)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len >= max_len) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return NULL;
    }
    char *buf = malloc(total_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return NULL;
    }
    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) { free(buf); return NULL; }
        received += ret;
    }
    buf[total_len] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }
    return root;
}

// 将 cJSON 对象序列化并发送为 JSON 响应，同时释放 root
static esp_err_t send_json_response(httpd_req_t *req, cJSON *root)
{
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ESP_OK;
}

// 发送 {"status":"ok"} 简单成功响应
static esp_err_t send_ok_response(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", -1);
    return ESP_OK;
}

static void copy_json_string(cJSON *obj, const char *name, char *dst, size_t dst_size)
{
    cJSON *value = cJSON_GetObjectItem(obj, name);
    if (cJSON_IsString(value)) safe_strcpy(dst, value->valuestring, dst_size);
}

static void push_channel_add_url_fields(cJSON *obj, const char *url)
{
    cJSON_AddStringToObject(obj, "url", url);
}

static void push_channel_add_url_token_fields(cJSON *obj, const char *url, const char *token)
{
    cJSON_AddStringToObject(obj, "url", url);
    cJSON_AddStringToObject(obj, "token", token);
}

static void push_channel_add_url_user_token_fields(cJSON *obj, const char *url, const char *user, const char *token)
{
    cJSON_AddStringToObject(obj, "url", url);
    cJSON_AddStringToObject(obj, "user", user);
    cJSON_AddStringToObject(obj, "token", token);
}

static void push_channel_add_url_secret_fields(cJSON *obj, const char *url, const char *secret)
{
    cJSON_AddStringToObject(obj, "url", url);
    cJSON_AddStringToObject(obj, "secret", secret);
}

static void push_channel_add_smtp_fields(cJSON *obj, const push_params_smtp_t *smtp)
{
    cJSON_AddStringToObject(obj, "url", smtp->server);
    cJSON_AddStringToObject(obj, "user", smtp->user);
    cJSON_AddStringToObject(obj, "password", smtp->password);
    cJSON_AddStringToObject(obj, "customBody", smtp->to);
    cJSON_AddStringToObject(obj, "port", smtp->port);
}

static void push_channel_parse_url(cJSON *item, char *url, size_t url_size)
{
    copy_json_string(item, "url", url, url_size);
}

static void push_channel_parse_url_token(cJSON *item, char *url, size_t url_size, char *token, size_t token_size)
{
    copy_json_string(item, "url", url, url_size);
    copy_json_string(item, "token", token, token_size);
}

static void push_channel_parse_url_user_token(cJSON *item, char *url, size_t url_size, char *user, size_t user_size, char *token, size_t token_size)
{
    copy_json_string(item, "url", url, url_size);
    copy_json_string(item, "user", user, user_size);
    copy_json_string(item, "token", token, token_size);
}

static void push_channel_parse_url_secret(cJSON *item, char *url, size_t url_size, char *secret, size_t secret_size)
{
    copy_json_string(item, "url", url, url_size);
    copy_json_string(item, "secret", secret, secret_size);
}

static void push_channel_parse_smtp(cJSON *item, push_params_smtp_t *smtp)
{
    copy_json_string(item, "url", smtp->server, sizeof(smtp->server));
    copy_json_string(item, "user", smtp->user, sizeof(smtp->user));
    copy_json_string(item, "password", smtp->password, sizeof(smtp->password));
    copy_json_string(item, "customBody", smtp->to, sizeof(smtp->to));
    copy_json_string(item, "port", smtp->port, sizeof(smtp->port));
}

static void push_channel_add_api_fields(cJSON *obj, const push_channel_t *ch)
{
    switch (ch->type) {
    case PUSH_TYPE_POST_JSON:
        push_channel_add_url_fields(obj, ch->params.post_json.url);
        cJSON_AddStringToObject(obj, "customBody", ch->params.post_json.body);
        break;
    case PUSH_TYPE_GET:
        push_channel_add_url_fields(obj, ch->params.get.url);
        break;
    case PUSH_TYPE_SMTP:
        push_channel_add_smtp_fields(obj, &ch->params.smtp);
        break;
    case PUSH_TYPE_BARK:
        push_channel_add_url_fields(obj, ch->params.bark.url);
        break;
    case PUSH_TYPE_DINGTALK:
        push_channel_add_url_secret_fields(obj, ch->params.dingtalk.url, ch->params.dingtalk.secret);
        break;
    case PUSH_TYPE_PUSHPLUS:
        push_channel_add_url_token_fields(obj, ch->params.pushplus.url, ch->params.pushplus.token);
        cJSON_AddStringToObject(obj, "channel", ch->params.pushplus.channel);
        break;
    case PUSH_TYPE_SERVERCHAN:
        push_channel_add_url_token_fields(obj, ch->params.serverchan.url, ch->params.serverchan.sendkey);
        break;
    case PUSH_TYPE_FEISHU:
        push_channel_add_url_secret_fields(obj, ch->params.feishu.url, ch->params.feishu.secret);
        break;
    case PUSH_TYPE_GOTIFY:
        push_channel_add_url_token_fields(obj, ch->params.gotify.url, ch->params.gotify.token);
        break;
    case PUSH_TYPE_TELEGRAM:
        push_channel_add_url_user_token_fields(obj, ch->params.telegram.url, ch->params.telegram.chat_id, ch->params.telegram.bot_token);
        break;
    case PUSH_TYPE_PUSHOVER:
        push_channel_add_url_user_token_fields(obj, ch->params.pushover.url, ch->params.pushover.user, ch->params.pushover.token);
        break;
    default:
        break;
    }
}

static void push_channel_from_api_json(push_channel_t *ch, cJSON *item)
{
    memset(&ch->params, 0, sizeof(ch->params));
    switch (ch->type) {
    case PUSH_TYPE_POST_JSON:
        push_channel_parse_url(item, ch->params.post_json.url, sizeof(ch->params.post_json.url));
        copy_json_string(item, "customBody", ch->params.post_json.body, sizeof(ch->params.post_json.body));
        break;
    case PUSH_TYPE_GET:
        push_channel_parse_url(item, ch->params.get.url, sizeof(ch->params.get.url));
        break;
    case PUSH_TYPE_SMTP:
        push_channel_parse_smtp(item, &ch->params.smtp);
        break;
    case PUSH_TYPE_BARK:
        push_channel_parse_url(item, ch->params.bark.url, sizeof(ch->params.bark.url));
        break;
    case PUSH_TYPE_DINGTALK:
        push_channel_parse_url_secret(item, ch->params.dingtalk.url, sizeof(ch->params.dingtalk.url), ch->params.dingtalk.secret, sizeof(ch->params.dingtalk.secret));
        break;
    case PUSH_TYPE_PUSHPLUS:
        push_channel_parse_url_token(item, ch->params.pushplus.url, sizeof(ch->params.pushplus.url), ch->params.pushplus.token, sizeof(ch->params.pushplus.token));
        copy_json_string(item, "channel", ch->params.pushplus.channel, sizeof(ch->params.pushplus.channel));
        break;
    case PUSH_TYPE_SERVERCHAN:
        push_channel_parse_url_token(item, ch->params.serverchan.url, sizeof(ch->params.serverchan.url), ch->params.serverchan.sendkey, sizeof(ch->params.serverchan.sendkey));
        break;
    case PUSH_TYPE_FEISHU:
        push_channel_parse_url_secret(item, ch->params.feishu.url, sizeof(ch->params.feishu.url), ch->params.feishu.secret, sizeof(ch->params.feishu.secret));
        break;
    case PUSH_TYPE_GOTIFY:
        push_channel_parse_url_token(item, ch->params.gotify.url, sizeof(ch->params.gotify.url), ch->params.gotify.token, sizeof(ch->params.gotify.token));
        break;
    case PUSH_TYPE_TELEGRAM:
        push_channel_parse_url_user_token(item, ch->params.telegram.url, sizeof(ch->params.telegram.url), ch->params.telegram.chat_id, sizeof(ch->params.telegram.chat_id), ch->params.telegram.bot_token, sizeof(ch->params.telegram.bot_token));
        break;
    case PUSH_TYPE_PUSHOVER:
        push_channel_parse_url_user_token(item, ch->params.pushover.url, sizeof(ch->params.pushover.url), ch->params.pushover.user, sizeof(ch->params.pushover.user), ch->params.pushover.token, sizeof(ch->params.pushover.token));
        break;
    default:
        break;
    }
}

static bool push_type_is_supported(int type)
{
    return type >= (int)PUSH_TYPE_POST_JSON && type <= (int)PUSH_TYPE_PUSHOVER;
}

/* ========== API 处理 ========== */

// 获取配置信息
static esp_err_t api_get_config_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    char ssid[33]={0}, pass[65]={0};
    config_get_wifi_sta(ssid, sizeof(ssid), pass, sizeof(pass));
    cJSON_AddStringToObject(root, "wifiSsid", ssid);
    cJSON_AddStringToObject(root, "wifiPass", pass); 
    
    return send_json_response(req, root);
}

// 保存配置信息
static esp_err_t api_post_config_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    cJSON *root = read_json_body(req, 8192);
    if (!root) return ESP_FAIL;

    cJSON *wifiSsid = cJSON_GetObjectItem(root, "wifiSsid");
    cJSON *wifiPass = cJSON_GetObjectItem(root, "wifiPass");
    if (cJSON_IsString(wifiSsid) && cJSON_IsString(wifiPass) && wifiSsid->valuestring && wifiPass->valuestring) {
        config_set_wifi_sta(wifiSsid->valuestring, wifiPass->valuestring);
    }
    
    cJSON_Delete(root);

    send_ok_response(req);
    
    // 延迟重启
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// GET /api/config/wifi：仅返回 ssid、password（网络由 DHCP 获取）
static esp_err_t api_get_config_wifi_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    char ssid[33] = {0}, pass[65] = {0};
    config_get_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddStringToObject(root, "password", pass);

    return send_json_response(req, root);
}

// POST /api/config/wifi：仅保存 ssid、password（网络由 DHCP 获取）
static esp_err_t api_post_config_wifi_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    cJSON *root = read_json_body(req, 2048);
    if (!root) return ESP_FAIL;

    const char *ssid = "";
    const char *password = "";
    cJSON *j;
    if ((j = cJSON_GetObjectItem(root, "ssid")) && cJSON_IsString(j)) ssid = j->valuestring;
    if ((j = cJSON_GetObjectItem(root, "password")) && cJSON_IsString(j)) password = j->valuestring;
    cJSON_Delete(root);

    if (!ssid) ssid = "";
    if (!password) password = "";
    config_set_wifi_sta(ssid, password);

    return send_ok_response(req);
}

// GET /api/config/auth：Basic 认证账户，返回 { user, hasPassword }，不返回真实密码
static esp_err_t api_get_config_auth_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "user", g_config.web_user);
    cJSON_AddBoolToObject(root, "hasPassword", strlen(g_config.web_pass) > 0);

    return send_json_response(req, root);
}

// POST /api/config/auth：保存 Basic 认证用户名与密码，body { user, password }，密码留空表示不修改
static esp_err_t api_post_config_auth_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    cJSON *root = read_json_body(req, 2048);
    if (!root) return ESP_FAIL;

    cJSON *j_user = cJSON_GetObjectItem(root, "user");
    cJSON *j_pass = cJSON_GetObjectItem(root, "password");
    if (cJSON_IsString(j_user) && j_user->valuestring) {
        strncpy(g_config.web_user, j_user->valuestring, sizeof(g_config.web_user) - 1);
        g_config.web_user[sizeof(g_config.web_user) - 1] = '\0';
    }
    if (cJSON_IsString(j_pass) && j_pass->valuestring && j_pass->valuestring[0] != '\0') {
        strncpy(g_config.web_pass, j_pass->valuestring, sizeof(g_config.web_pass) - 1);
        g_config.web_pass[sizeof(g_config.web_pass) - 1] = '\0';
    }
    cJSON_Delete(root);

    config_save();

    return send_ok_response(req);
}

// GET /api/config/push：返回 { "channels": [ ... ] }
static esp_err_t api_get_config_push_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    cJSON *channels = cJSON_CreateArray();
    for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
        const push_channel_t *ch = &g_config.push_channels[i];
        if (!config_is_push_channel_valid(ch)) continue;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(obj, "enabled", ch->enabled);
        cJSON_AddNumberToObject(obj, "type", (double)ch->type);
        cJSON_AddStringToObject(obj, "name", ch->name);
        push_channel_add_api_fields(obj, ch);
        cJSON_AddItemToArray(channels, obj);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "channels", channels);

    return send_json_response(req, root);
}

// POST /api/config/push：body { "channels": [ ... ] }，不重启
static esp_err_t api_post_config_push_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    cJSON *root = read_json_body(req, 8192);
    if (!root) return ESP_FAIL;

    cJSON *arr = cJSON_GetObjectItem(root, "channels");
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        if (n > MAX_PUSH_CHANNELS) n = MAX_PUSH_CHANNELS;
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(arr, i);
            if (!cJSON_IsObject(item)) continue;
            push_channel_t *ch = &g_config.push_channels[i];
            memset(ch, 0, sizeof(push_channel_t));
            cJSON *j;
            if ((j = cJSON_GetObjectItem(item, "enabled"))) ch->enabled = cJSON_IsTrue(j);
            j = cJSON_GetObjectItem(item, "type");
            if (!cJSON_IsNumber(j) || !push_type_is_supported((int)j->valuedouble)) {
                cJSON_Delete(root);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid push type");
                return ESP_OK;
            }
            ch->type = (push_type_t)(int)j->valuedouble;
            copy_json_string(item, "name", ch->name, sizeof(ch->name));
            push_channel_from_api_json(ch, item);
        }
        // 未在请求中的槽位清空，避免删除通道后 GET 仍返回旧数据
        for (int i = n; i < MAX_PUSH_CHANNELS; i++) {
            memset(&g_config.push_channels[i], 0, sizeof(push_channel_t));
        }
        config_save();
    }
    cJSON_Delete(root);

    return send_ok_response(req);
}

// GET /api/config/schedule：返回 { "tasks": [ ... ] }，最多 3 条
static esp_err_t api_get_config_schedule_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    cJSON *tasks = cJSON_CreateArray();
    for (int i = 0; i < MAX_SCHEDULE_TASKS; i++) {
        const schedule_task_t *t = &g_schedule_tasks[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(obj, "enabled", t->enabled);
        cJSON_AddNumberToObject(obj, "action", (double)t->action);
        cJSON_AddNumberToObject(obj, "kind", (double)t->kind);
        cJSON_AddNumberToObject(obj, "hour", t->hour);
        cJSON_AddNumberToObject(obj, "minute", t->minute);
        cJSON_AddNumberToObject(obj, "weekdayMask", t->weekday_mask);
        cJSON_AddNumberToObject(obj, "monthInterval", t->month_interval);
        cJSON_AddNumberToObject(obj, "initialTs", (double)(int64_t)t->initial_ts);
        cJSON_AddStringToObject(obj, "phone", t->phone);
        cJSON_AddStringToObject(obj, "message", t->message);
        cJSON_AddStringToObject(obj, "pingHost", t->ping_host);
        cJSON_AddNumberToObject(obj, "lastRunTs", (double)(int64_t)t->last_run_ts);
        cJSON_AddBoolToObject(obj, "lastOk", t->last_ok);
        cJSON_AddStringToObject(obj, "lastMsg", t->last_msg);
        cJSON_AddItemToArray(tasks, obj);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "tasks", tasks);

    return send_json_response(req, root);
}

// POST /api/config/schedule：body { "tasks": [ ... ] }，保存定时任务配置（不覆盖 lastRunTs/lastOk/lastMsg 可由设备只读）
static esp_err_t api_post_config_schedule_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    cJSON *root = read_json_body(req, 4096);
    if (!root) return ESP_FAIL;

    cJSON *arr = cJSON_GetObjectItem(root, "tasks");
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        if (n > MAX_SCHEDULE_TASKS) n = MAX_SCHEDULE_TASKS;
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(arr, i);
            if (!cJSON_IsObject(item)) continue;
            schedule_task_t *t = &g_schedule_tasks[i];
            cJSON *j;
            if ((j = cJSON_GetObjectItem(item, "enabled"))) t->enabled = cJSON_IsTrue(j);
            if ((j = cJSON_GetObjectItem(item, "action")) && cJSON_IsNumber(j)) t->action = (schedule_action_t)(int)j->valuedouble;
            if ((j = cJSON_GetObjectItem(item, "kind")) && cJSON_IsNumber(j)) t->kind = (schedule_kind_t)(int)j->valuedouble;
            if ((j = cJSON_GetObjectItem(item, "hour")) && cJSON_IsNumber(j)) t->hour = (uint8_t)(int)j->valuedouble;
            if ((j = cJSON_GetObjectItem(item, "minute")) && cJSON_IsNumber(j)) t->minute = (uint8_t)(int)j->valuedouble;
            if ((j = cJSON_GetObjectItem(item, "weekdayMask")) && cJSON_IsNumber(j)) t->weekday_mask = (uint8_t)(int)j->valuedouble;
            if ((j = cJSON_GetObjectItem(item, "monthInterval")) && cJSON_IsNumber(j)) { int v = (int)j->valuedouble; t->month_interval = (v >= 1 && v <= 12) ? (uint8_t)v : 1; }
            if ((j = cJSON_GetObjectItem(item, "initialTs")) && cJSON_IsNumber(j)) { int64_t v = (int64_t)j->valuedouble; t->initial_ts = (time_t)v; }
            if ((j = cJSON_GetObjectItem(item, "phone")) && cJSON_IsString(j)) strncpy(t->phone, j->valuestring, sizeof(t->phone) - 1);
            if ((j = cJSON_GetObjectItem(item, "message")) && cJSON_IsString(j)) strncpy(t->message, j->valuestring, sizeof(t->message) - 1);
            if ((j = cJSON_GetObjectItem(item, "pingHost")) && cJSON_IsString(j)) strncpy(t->ping_host, j->valuestring, sizeof(t->ping_host) - 1);
        }
        schedule_save();
    }
    cJSON_Delete(root);

    return send_ok_response(req);
}

// POST /api/push/test：body { "index": number }，测试指定推送通道
static esp_err_t api_post_push_test_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    cJSON *root = read_json_body(req, 512);
    if (!root) return ESP_FAIL;

    cJSON *j_index = cJSON_GetObjectItem(root, "index");
    int index = (j_index && cJSON_IsNumber(j_index)) ? (int)j_index->valuedouble : 0;
    cJSON_Delete(root);

    if (index < 0 || index >= MAX_PUSH_CHANNELS) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"message\":\"无效的通道索引\"}", -1);
        return ESP_OK;
    }

    push_channel_t *ch = &g_config.push_channels[index];
    if (!ch->enabled || !config_is_push_channel_valid(ch)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"message\":\"通道未启用或配置不完整\"}", -1);
        return ESP_OK;
    }

    time_t now = time(NULL);
    char ts[32];
    snprintf(ts, sizeof(ts), "%lld", (long long)now);
    push_send_to_channel(ch, "Test", "这是一条测试推送", ts);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"message\":\"测试成功\"}", -1);
    return ESP_OK;
}

// GET /api/debug/memory：返回当前内存占用与剩余（字节），供日志页展示
static esp_err_t api_get_debug_memory_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    size_t free_bytes = esp_get_free_heap_size();
    size_t min_free = esp_get_minimum_free_heap_size();
    size_t total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    size_t used = (total > free_bytes) ? (total - free_bytes) : 0;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "free", (double)free_bytes);
    cJSON_AddNumberToObject(root, "total", (double)total);
    cJSON_AddNumberToObject(root, "min_free", (double)min_free);
    cJSON_AddNumberToObject(root, "used", (double)used);
    return send_json_response(req, root);
}

// POST /api/debug/ping：4G 模组 Ping 测试（ping 前检查 CGACT 并临时开启，测试完成后关闭）
static esp_err_t api_post_debug_ping_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    ESP_LOGI(TAG, "收到前端 Ping 测试请求");

    char msg[128];
    msg[0] = '\0';

    // 检查 PDP 是否已激活，未激活则先激活
    bool was_active = modem_cgact_is_active(1);
    if (!was_active) {
        ESP_LOGI(TAG, "当前 CGACT: context 1 未激活，准备通过 CGACT=1,1 启用数据连接");
        if (!modem_cgact_activate(1)) {
            snprintf(msg, sizeof(msg), "无法激活数据连接 (CGACT=1,1)");
            ESP_LOGW(TAG, "Ping 测试前激活 CGACT 失败");
            cJSON *root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "success", false);
            cJSON_AddStringToObject(root, "message", msg);
            send_json_response(req, root);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2000)); // 等待 PDP 就绪
        ESP_LOGI(TAG, "CGACT 激活完成，延时 2s 等待网络就绪后开始 MPING");
    } else {
        ESP_LOGI(TAG, "当前 CGACT: context 1 已激活，直接进行 MPING 测试");
    }

    bool ok = modem_ping("8.8.8.8", msg, sizeof(msg));
    ESP_LOGI(TAG, "Ping 测试结束，ok=%d, msg=%s", ok, msg);

    // 测试前是我们临时开启的，测试完成后关闭数据连接
    if (!was_active) {
        ESP_LOGI(TAG, "测试完成，关闭临时开启的 CGACT 数据连接");
        modem_cgact_deactivate(1);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", ok);
    cJSON_AddStringToObject(root, "message", msg);
    return send_json_response(req, root);
}

// GET /api/sms/history?offset=&limit=：从内存环形缓冲读取最近若干条（最多 5 条），分页返回
#define SMS_HISTORY_RING_MAX  5
static esp_err_t api_get_sms_history_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    int offset = 0, limit = 20;
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "offset", val, sizeof(val)) == ESP_OK) offset = atoi(val);
        if (httpd_query_key_value(query, "limit", val, sizeof(val)) == ESP_OK) limit = atoi(val);
    }
    if (offset < 0) offset = 0;
    if (limit <= 0 || limit > SMS_HISTORY_RING_MAX) limit = SMS_HISTORY_RING_MAX;

    sms_history_entry_t list[SMS_HISTORY_RING_MAX];
    int total = sms_history_get(list, SMS_HISTORY_RING_MAX);
    int start = offset;
    int end = offset + limit;
    if (start > total) start = total;
    if (end > total) end = total;

    cJSON *arr = cJSON_CreateArray();
    for (int i = start; i < end; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id", list[i].id);
        cJSON_AddNumberToObject(obj, "ts", list[i].ts);
        cJSON_AddStringToObject(obj, "sender", list[i].sender);
        cJSON_AddStringToObject(obj, "content", list[i].content);
        cJSON_AddItemToArray(arr, obj);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "list", arr);
    cJSON_AddNumberToObject(root, "total", total);

    return send_json_response(req, root);
}

// 短信发送异步：避免在 httpd 单任务中长时间阻塞（且 WebSocket 日志会占用该任务）
#define SMS_ASYNC_PHONE_LEN  32
#define SMS_ASYNC_MSG_LEN    512
static struct {
    httpd_req_t *async_req;
    char phone[SMS_ASYNC_PHONE_LEN];
    char message[SMS_ASYNC_MSG_LEN];
} s_sms_async;
static SemaphoreHandle_t s_sms_async_mux;

static void sms_send_task(void *arg)
{
    (void)arg;
    bool ok = sms_send(s_sms_async.phone, s_sms_async.message);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", ok);
    cJSON_AddStringToObject(resp, "message", ok ? "发送成功" : "发送失败");
    cJSON_AddStringToObject(resp, "status", ok ? "ok" : "fail");
    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (json_str) {
        httpd_resp_set_type(s_sms_async.async_req, "application/json");
        httpd_resp_send(s_sms_async.async_req, json_str, strlen(json_str));
        free(json_str);
    } else {
        httpd_resp_send_err(s_sms_async.async_req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    httpd_req_async_handler_complete(s_sms_async.async_req);
    xSemaphoreGive(s_sms_async_mux);
    vTaskDelete(NULL);
}

// 发送短信请求（异步：在独立任务中执行 sms_send，不阻塞 httpd 单任务）
static esp_err_t api_post_sms_send_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "[api/sms/send] handler 进入");
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) {
        ESP_LOGI(TAG, "[api/sms/send] 未通过认证，返回 401");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "收到前端短信发送请求 /api/sms/send");

    int total_len = req->content_len;
    ESP_LOGI(TAG, "请求 Content-Length=%d", total_len);
    if (total_len <= 0 || total_len >= 8192) {
        ESP_LOGW(TAG, "请求体长度非法，拒绝处理");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_OK;
    }

    char *buf = malloc(total_len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "内存不足，无法分配请求缓冲区");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_OK;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            ESP_LOGW(TAG, "httpd_req_recv 失败 ret=%d，已接收=%d/%d", ret, received, total_len);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Read body failed");
            return ESP_OK;
        }
        received += ret;
    }
    buf[total_len] = '\0';
    ESP_LOGD(TAG, "请求原始 JSON: %s", buf);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "JSON 解析失败");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    cJSON *phone = cJSON_GetObjectItem(root, "phone");
    cJSON *message = cJSON_GetObjectItem(root, "message");

    if (!cJSON_IsString(phone) || !cJSON_IsString(message) ||
        !phone->valuestring || !message->valuestring) {
        ESP_LOGW(TAG, "JSON 中缺少 phone/message 字段或类型错误");
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing phone or message");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "准备发送短信，目标=%s，内容=%s", phone->valuestring, message->valuestring);

    if (xSemaphoreTake(s_sms_async_mux, 0) != pdTRUE) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "已有短信发送在进行中，拒绝并发");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SMS send busy");
        return ESP_OK;
    }

    snprintf(s_sms_async.phone, sizeof(s_sms_async.phone), "%.31s", phone->valuestring);
    snprintf(s_sms_async.message, sizeof(s_sms_async.message), "%.511s", message->valuestring);
    cJSON_Delete(root);

    httpd_req_t *async_req = NULL;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        xSemaphoreGive(s_sms_async_mux);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Async begin failed");
        return ESP_OK;
    }
    s_sms_async.async_req = async_req;

    if (xTaskCreate(sms_send_task, "sms_send", 4096, NULL, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
        httpd_req_async_handler_complete(async_req);
        xSemaphoreGive(s_sms_async_mux);
        ESP_LOGE(TAG, "创建短信发送任务失败");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "短信发送已提交到后台任务");
    return ESP_OK;
}

// 解析 +CESQ: rxlev,ber,rscp,ecno,rsrq,rsrp 中第 field_idx 个整数字段（0起）
static int cesq_field(const char *params, int field_idx)
{
    int idx = 0;
    const char *p = params;
    while (*p) {
        if (idx == field_idx) {
            return atoi(p);
        }
        const char *comma = strchr(p, ',');
        if (!comma) break;
        p = comma + 1;
        idx++;
    }
    return 99;
}

/* 运营商 MCC+MNC 数字编码 → 中文名称（常见国内编码） */
static const char *oper_code_to_name(const char *code)
{
    static const struct { const char *code; const char *name; } tbl[] = {
        {"46000", "中国移动"}, {"46002", "中国移动"},
        {"46007", "中国移动"}, {"46008", "中国移动"},
        {"46001", "中国联通"}, {"46006", "中国联通"}, {"46009", "中国联通"},
        {"46003", "中国电信"}, {"46005", "中国电信"}, {"46011", "中国电信"},
        {"46020", "中国铁通"}, {"46004", "中国卫通"},
        {NULL, NULL}
    };
    for (int i = 0; tbl[i].code; i++) {
        if (strcmp(code, tbl[i].code) == 0) return tbl[i].name;
    }
    return NULL;
}

// 模组状态查询接口：信号质量 + 网络注册 + 运营商
static esp_err_t api_get_modem_status_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    char *buf = malloc(512);
    if (!buf) return ESP_FAIL;

    cJSON *root    = cJSON_CreateObject();
    // 固件标识：编译时间戳，便于前端/接口确认当前运行固件
    cJSON_AddStringToObject(root, "fw_build", g_fw_build);
    cJSON *signal  = cJSON_AddObjectToObject(root, "signal");
    cJSON *network = cJSON_AddObjectToObject(root, "network");

    /* ---- 信号质量: AT+CESQ ---- */
    modem_send_at("AT+CESQ", buf, 511, 3000);
    char *cesq_p = strstr(buf, "+CESQ:");
    if (cesq_p) {
        cesq_p += 6;
        while (*cesq_p == ' ') cesq_p++;
        char params[64] = {0};
        int end = strcspn(cesq_p, "\r\n");
        if (end > 0 && end < 63) { strncpy(params, cesq_p, end); }

        int rsrp_raw = cesq_field(params, 5);
        int rsrq_raw = cesq_field(params, 4);

        if (rsrp_raw == 99 || rsrp_raw == 255) {
            cJSON_AddStringToObject(signal, "rsrp_str", "未知");
            cJSON_AddStringToObject(signal, "quality",  "未知");
            cJSON_AddNumberToObject(signal, "rsrp_dbm", -999);
            cJSON_AddNumberToObject(signal, "level",    0);
        } else {
            int rsrp_dbm = -140 + rsrp_raw;
            const char *quality;
            int level;
            if      (rsrp_dbm >= -80)  { quality = "极好"; level = 5; }
            else if (rsrp_dbm >= -90)  { quality = "良好"; level = 4; }
            else if (rsrp_dbm >= -100) { quality = "一般"; level = 3; }
            else if (rsrp_dbm >= -110) { quality = "较弱"; level = 2; }
            else                       { quality = "很差"; level = 1; }
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%d dBm", rsrp_dbm);
            cJSON_AddStringToObject(signal, "rsrp_str", tmp);
            cJSON_AddStringToObject(signal, "quality",  quality);
            cJSON_AddNumberToObject(signal, "rsrp_dbm", rsrp_dbm);
            cJSON_AddNumberToObject(signal, "level",    level);
        }

        if (rsrq_raw == 99 || rsrq_raw == 255) {
            cJSON_AddStringToObject(signal, "rsrq_str", "未知");
        } else {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%.1f dB", -19.5f + rsrq_raw * 0.5f);
            cJSON_AddStringToObject(signal, "rsrq_str", tmp);
        }
    } else {
        cJSON_AddStringToObject(signal, "rsrp_str", "查询失败");
        cJSON_AddStringToObject(signal, "rsrq_str", "查询失败");
        cJSON_AddStringToObject(signal, "quality",  "未知");
        cJSON_AddNumberToObject(signal, "rsrp_dbm", -999);
        cJSON_AddNumberToObject(signal, "level",    0);
    }

    /* ---- 网络注册: AT+CEREG? ---- */
    modem_send_at("AT+CEREG?", buf, 511, 3000);
    char *cereg_p = strstr(buf, "+CEREG:");
    if (cereg_p) {
        cereg_p += 7;
        char *comma = strchr(cereg_p, ',');
        int stat = comma ? atoi(comma + 1) : -1;
        const char *reg_status;
        bool registered;
        switch (stat) {
        case 1:  reg_status = "已注册（本地）"; registered = true;  break;
        case 5:  reg_status = "已注册（漫游）"; registered = true;  break;
        case 2:  reg_status = "正在搜索";       registered = false; break;
        case 3:  reg_status = "注册被拒绝";     registered = false; break;
        case 0:  reg_status = "未注册";         registered = false; break;
        default: reg_status = "未知";           registered = false; break;
        }
        cJSON_AddStringToObject(network, "reg_status", reg_status);
        cJSON_AddBoolToObject(network, "registered", registered);
    } else {
        cJSON_AddStringToObject(network, "reg_status", "查询失败");
        cJSON_AddBoolToObject(network, "registered", false);
    }

    /* ---- 运营商: AT+COPS? ---- */
    modem_send_at("AT+COPS?", buf, 511, 3000);
    char *cops_p = strstr(buf, "+COPS:");
    if (cops_p) {
        char *q1 = strchr(cops_p, '"');
        if (q1) {
            char *q2 = strchr(q1 + 1, '"');
            if (q2 && q2 > q1) {
                char oper[64] = {0};
                int len = (int)(q2 - q1 - 1);
                if (len > 0 && len < 63) strncpy(oper, q1 + 1, len);
                /* 若为纯数字编码（MCC+MNC），查表转为中文名称 */
                const char *name = oper_code_to_name(oper);
                cJSON_AddStringToObject(network, "operator", name ? name : oper);
            } else {
                cJSON_AddStringToObject(network, "operator", "未知");
            }
        } else {
            cJSON_AddStringToObject(network, "operator", "未注册");
        }
    } else {
        cJSON_AddStringToObject(network, "operator", "查询失败");
    }

    free(buf);

    /* ---- WiFi 状态 ---- */
    cJSON *wifi_obj = cJSON_AddObjectToObject(root, "wifi");
    wifi_info_t winfo;
    wifi_mgr_get_info(&winfo);
    bool ap_mode = wifi_mgr_is_ap_mode();

    if (ap_mode) {
        cJSON_AddStringToObject(wifi_obj, "mode",    "AP");
        cJSON_AddStringToObject(wifi_obj, "ssid",    "SMS-Forwarder-Setup");
        cJSON_AddStringToObject(wifi_obj, "ip",      "192.168.4.1");
        cJSON_AddStringToObject(wifi_obj, "status",  "热点模式");
        cJSON_AddBoolToObject  (wifi_obj, "connected", false);
        cJSON_AddNumberToObject(wifi_obj, "rssi",    0);
        cJSON_AddNumberToObject(wifi_obj, "channel", 0);
    } else {
        cJSON_AddStringToObject(wifi_obj, "mode",      "STA");
        cJSON_AddBoolToObject  (wifi_obj, "connected", winfo.connected);
        cJSON_AddStringToObject(wifi_obj, "ssid",      winfo.ssid);
        cJSON_AddStringToObject(wifi_obj, "ip",        winfo.ip);
        cJSON_AddStringToObject(wifi_obj, "gateway",   winfo.gateway);
        cJSON_AddStringToObject(wifi_obj, "dns",       winfo.dns);
        cJSON_AddStringToObject(wifi_obj, "mac",       winfo.mac);
        cJSON_AddNumberToObject(wifi_obj, "rssi",      winfo.rssi);
        cJSON_AddNumberToObject(wifi_obj, "channel",   winfo.channel);
        /* RSSI → 强度文字 */
        const char *rssi_str;
        if      (!winfo.connected)  rssi_str = "未连接";
        else if (winfo.rssi >= -55) rssi_str = "极强";
        else if (winfo.rssi >= -65) rssi_str = "强";
        else if (winfo.rssi >= -75) rssi_str = "中";
        else                        rssi_str = "弱";
        cJSON_AddStringToObject(wifi_obj, "status", rssi_str);
    }

    return send_json_response(req, root);
}

// AT指令测试接口
static esp_err_t api_post_at_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    cJSON *root = read_json_body(req, 512);
    if (!root) return ESP_FAIL;

    cJSON *cmd_item     = cJSON_GetObjectItem(root, "cmd");
    cJSON *timeout_item = cJSON_GetObjectItem(root, "timeout");

    if (!cJSON_IsString(cmd_item) || !cmd_item->valuestring || strlen(cmd_item->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing cmd");
        return ESP_FAIL;
    }

    int timeout_ms = 3000;
    if (cJSON_IsNumber(timeout_item)) {
        timeout_ms = (int)timeout_item->valuedouble;
        if (timeout_ms < 100)   timeout_ms = 100;
        if (timeout_ms > 10000) timeout_ms = 10000;
    }

    // 复制指令字符串，释放 cJSON 对象
    char cmd[256] = {0};
    strncpy(cmd, cmd_item->valuestring, sizeof(cmd) - 1);
    cJSON_Delete(root);

    char *resp_buf = malloc(1024);
    char *echo_buf = malloc(1536);
    if (!resp_buf || !echo_buf) {
        free(resp_buf);
        free(echo_buf);
        return ESP_FAIL;
    }

    // 执行 AT 并获取原始响应
    int resp_len = modem_send_at(cmd, resp_buf, 1023, timeout_ms);
    if (resp_len < 0) resp_len = 0;
    resp_buf[resp_len] = '\0';

    // 获取当前时间字符串
    char ts[32] = {0};
    time_t now = time(NULL);
    struct tm tm_info;
    if (localtime_r(&now, &tm_info)) {
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_info);
    } else {
        strncpy(ts, "0000-00-00 00:00:00", sizeof(ts) - 1);
    }

    // 压缩输出中的换行：\r\n / \r -> \n，连续空行折叠为一个\n
    char compressed[1024];
    size_t w = 0;
    bool last_nl = false;
    for (size_t i = 0; i < (size_t)resp_len && w < sizeof(compressed) - 1; i++) {
        char c = resp_buf[i];
        if (c == '\r') {
            // 统一处理为 \n，并跳过紧跟的 \n
            if (!last_nl) {
                compressed[w++] = '\n';
                last_nl = true;
            }
            if (i + 1 < (size_t)resp_len && resp_buf[i + 1] == '\n') {
                i++;
            }
        } else if (c == '\n') {
            if (!last_nl) {
                compressed[w++] = '\n';
                last_nl = true;
            }
        } else {
            compressed[w++] = c;
            last_nl = false;
        }
    }
    compressed[w] = '\0';

    // 去掉前导换行，避免只返回 "\nOK\n" 时界面看起来像是空行
    char *out_text = compressed;
    while (*out_text == '\n') out_text++;

    // 生成统一回显格式：[时间]>>> 输入\n[时间]<<< 输出
    snprintf(echo_buf, 1536, "[%s]>>> %s\n[%s]<<< %s", ts, cmd, ts, out_text);

    ESP_LOGI(TAG, "%s", echo_buf);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "response", echo_buf);
    free(resp_buf);
    free(echo_buf);

    return send_json_response(req, result);
}

/* ========== 前端静态文件路由 ========== */
static esp_err_t index_html_handler(httpd_req_t *req)
{
    if (strlen(g_config.web_user) > 0 && !check_auth(req)) return ESP_OK;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    
    size_t size = index_html_gz_end - index_html_gz_start;
    httpd_resp_send(req, (const char *)index_html_gz_start, size);
    return ESP_OK;
}

#if defined(CONFIG_HTTPD_WS_SUPPORT) && CONFIG_HTTPD_WS_SUPPORT
/* ========== 日志 WebSocket：esp_log 实时推送到前端 ========== */
#define LOG_WS_RING_LEN   64
#define LOG_WS_LINE_MAX   256
#define LOG_WS_QUEUE_LEN  64

static char s_log_ring[LOG_WS_RING_LEN][LOG_WS_LINE_MAX];
static uint8_t s_log_ring_head;
static QueueHandle_t s_log_queue;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig_log_vprintf;

static int log_vprintf_ws(const char *fmt, va_list args)
{
    va_list args2;
    va_copy(args2, args);
    char line[LOG_WS_LINE_MAX];
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n <= 0) { va_end(args2); return n; }
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
    line[n] = '\0';
    if (s_orig_log_vprintf) s_orig_log_vprintf(fmt, args2);
    va_end(args2);

    if (s_log_queue) {
        taskENTER_CRITICAL(&s_log_mux);
        uint8_t idx = s_log_ring_head;
        strncpy(s_log_ring[idx], line, LOG_WS_LINE_MAX - 1);
        s_log_ring[idx][LOG_WS_LINE_MAX - 1] = '\0';
        s_log_ring_head = (s_log_ring_head + 1) % LOG_WS_RING_LEN;
        taskEXIT_CRITICAL(&s_log_mux);
        xQueueSend(s_log_queue, &idx, 0);
    }
    return n;
}

// WebSocket 日志循环在独立任务中运行，避免永久占用 httpd 单任务导致 /api/sms/send 等无法被调度
static void ws_log_loop_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    uint8_t idx;
    while (1) {
        if (xQueueReceive(s_log_queue, &idx, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (idx < LOG_WS_RING_LEN) {
                httpd_ws_frame_t ws_pkt = {
                    .final = true,
                    .fragmented = false,
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t *)s_log_ring[idx],
                    .len = strlen(s_log_ring[idx]),
                };
                if (httpd_ws_send_frame(req, &ws_pkt) != ESP_OK) break;
            }
        }
        httpd_ws_frame_t recv_pkt;
        if (httpd_ws_recv_frame(req, &recv_pkt, 0) == ESP_OK) {
            if (recv_pkt.type == HTTPD_WS_TYPE_CLOSE) break;
        }
    }
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t ws_log_handler(httpd_req_t *req)
{
    /* 不在此处做 Basic 认证：部分浏览器对同源 WebSocket 不自动带 Authorization，会导致设备上无法建连；日志流仅只读 */
    if (req->method == HTTP_GET) {
        httpd_req_t *async_req = NULL;
        if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
            return ESP_FAIL;
        }
        if (xTaskCreate(ws_log_loop_task, "ws_log", 4096, async_req, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
            httpd_req_async_handler_complete(async_req);
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    return ESP_FAIL;
}
#endif /* CONFIG_HTTPD_WS_SUPPORT */

/* ========== 服务器启停 ========== */

static httpd_handle_t s_server = NULL;

httpd_handle_t web_server_start(void)
{
    if (s_server != NULL) return s_server;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24;
    config.stack_size = 20480;        // AT 回显 compressed[1024]+snprintf/vfprintf、JSON 构建等栈需求大，避免 Stack protection fault
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_open_sockets = 6;      // 为 WebSocket /ws/log 及多请求留足连接，避免设备上 WS 无法建连
    config.recv_wait_timeout = 60;    // 默认 5s，发短信等长耗时 handler 期间避免 recv 超时断连
    config.send_wait_timeout = 60;    // 默认 5s，长耗时后发送响应时避免 send 超时
    config.lru_purge_enable = true;   // 占满时剔除最久未用连接，新请求仍可接入

#if defined(CONFIG_HTTPD_WS_SUPPORT) && CONFIG_HTTPD_WS_SUPPORT
    if (!s_log_queue) {
        s_log_queue = xQueueCreate(LOG_WS_QUEUE_LEN, sizeof(uint8_t));
        s_orig_log_vprintf = esp_log_set_vprintf(log_vprintf_ws);
    }
#endif

    // 使用二值信号量表示“短信发送槽位”：同一时刻只允许一次发送。
    // 必须用二值信号量而非互斥量：take 在 HTTP 任务、give 在 sms_send_task，
    // 互斥量要求同一任务 take/give，否则会触发 FreeRTOS 优先级继承断言崩溃。
    if (s_sms_async_mux == NULL) {
        s_sms_async_mux = xSemaphoreCreateBinary();
        if (s_sms_async_mux != NULL) {
            xSemaphoreGive(s_sms_async_mux);  // 初始为“槽位可用”
        }
    }
    ESP_LOGI(TAG, "启动 Web 服务器，端口: %d", config.server_port);
    if (httpd_start(&s_server, &config) == ESP_OK) {

#if defined(CONFIG_HTTPD_WS_SUPPORT) && CONFIG_HTTPD_WS_SUPPORT
        /* 最先注册 /ws/log，确保由 WebSocket 处理而非被通配路由抢走 */
        httpd_uri_t uri_ws_log = {
            .uri      = "/ws/log",
            .method   = HTTP_GET,
            .handler  = ws_log_handler,
            .is_websocket = true,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_ws_log);
        ESP_LOGI(TAG, "已注册 WebSocket /ws/log");
#endif

        httpd_uri_t uri_api_get_config = {
            .uri      = "/api/config",
            .method   = HTTP_GET,
            .handler  = api_get_config_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_get_config);

        httpd_uri_t uri_api_post_config = {
            .uri      = "/api/config",
            .method   = HTTP_POST,
            .handler  = api_post_config_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_post_config);

        httpd_uri_t uri_api_post_sms = {
            .uri      = "/api/sms/send",
            .method   = HTTP_POST,
            .handler  = api_post_sms_send_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_post_sms);

        httpd_uri_t uri_api_at = {
            .uri      = "/api/at",
            .method   = HTTP_POST,
            .handler  = api_post_at_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_at);

        httpd_uri_t uri_api_modem_status = {
            .uri      = "/api/modem/status",
            .method   = HTTP_GET,
            .handler  = api_get_modem_status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_modem_status);

        httpd_uri_t uri_api_status_network = {
            .uri      = "/api/status/network",
            .method   = HTTP_GET,
            .handler  = api_get_modem_status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_status_network);

        httpd_uri_t uri_api_get_config_wifi = {
            .uri      = "/api/config/wifi",
            .method   = HTTP_GET,
            .handler  = api_get_config_wifi_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_get_config_wifi);

        httpd_uri_t uri_api_post_config_wifi = {
            .uri      = "/api/config/wifi",
            .method   = HTTP_POST,
            .handler  = api_post_config_wifi_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_post_config_wifi);

        httpd_uri_t uri_api_get_config_auth = {
            .uri      = "/api/config/auth",
            .method   = HTTP_GET,
            .handler  = api_get_config_auth_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_get_config_auth);

        httpd_uri_t uri_api_post_config_auth = {
            .uri      = "/api/config/auth",
            .method   = HTTP_POST,
            .handler  = api_post_config_auth_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_post_config_auth);

        httpd_uri_t uri_api_get_config_push = {
            .uri      = "/api/config/push",
            .method   = HTTP_GET,
            .handler  = api_get_config_push_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_get_config_push);

        httpd_uri_t uri_api_post_config_push = {
            .uri      = "/api/config/push",
            .method   = HTTP_POST,
            .handler  = api_post_config_push_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_post_config_push);

        httpd_uri_t uri_api_get_config_schedule = {
            .uri      = "/api/config/schedule",
            .method   = HTTP_GET,
            .handler  = api_get_config_schedule_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_get_config_schedule);

        httpd_uri_t uri_api_post_config_schedule = {
            .uri      = "/api/config/schedule",
            .method   = HTTP_POST,
            .handler  = api_post_config_schedule_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_post_config_schedule);

        httpd_uri_t uri_api_post_push_test = {
            .uri      = "/api/push/test",
            .method   = HTTP_POST,
            .handler  = api_post_push_test_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_post_push_test);

        httpd_uri_t uri_api_get_sms_history = {
            .uri      = "/api/sms/history",
            .method   = HTTP_GET,
            .handler  = api_get_sms_history_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_get_sms_history);

        httpd_uri_t uri_api_get_debug_memory = {
            .uri      = "/api/debug/memory",
            .method   = HTTP_GET,
            .handler  = api_get_debug_memory_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_get_debug_memory);

        httpd_uri_t uri_api_debug_ping = {
            .uri      = "/api/debug/ping",
            .method   = HTTP_POST,
            .handler  = api_post_debug_ping_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_api_debug_ping);

        // 首页及所有其他路由
        httpd_uri_t uri_index = {
            .uri      = "/*",
            .method   = HTTP_GET,
            .handler  = index_html_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_index);

    } else {
        ESP_LOGE(TAG, "Web 服务器启动失败");
    }
    return s_server;
}

void web_server_stop(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
        if (server == s_server) {
            s_server = NULL;
        }
    }
}