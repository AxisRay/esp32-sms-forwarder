#include "config.h"
#include "utils.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "config";

/** 旧格式迁移：从扁平键 (url, k1, k2, body, port) 按 type 填入 union */
static void push_migrate_legacy_to_union(push_channel_t *ch, const char *url, const char *k1, const char *k2, const char *body, const char *port)
{
    memset(&ch->params, 0, sizeof(ch->params));
    switch (ch->type) {
    case PUSH_TYPE_POST_JSON:
        safe_strcpy(ch->params.post_json.url, url, sizeof(ch->params.post_json.url));
        safe_strcpy(ch->params.post_json.body, body, sizeof(ch->params.post_json.body));
        break;
    case PUSH_TYPE_BARK:
    case PUSH_TYPE_GET:
        safe_strcpy(ch->params.bark.url, url, sizeof(ch->params.bark.url));
        break;
    case PUSH_TYPE_DINGTALK:
        safe_strcpy(ch->params.dingtalk.url, url, sizeof(ch->params.dingtalk.url));
        safe_strcpy(ch->params.dingtalk.secret, k1, sizeof(ch->params.dingtalk.secret));
        break;
    case PUSH_TYPE_PUSHPLUS:
        safe_strcpy(ch->params.pushplus.url, url, sizeof(ch->params.pushplus.url));
        safe_strcpy(ch->params.pushplus.token, k1, sizeof(ch->params.pushplus.token));
        safe_strcpy(ch->params.pushplus.channel, k2, sizeof(ch->params.pushplus.channel));
        break;
    case PUSH_TYPE_SERVERCHAN:
        safe_strcpy(ch->params.serverchan.url, url, sizeof(ch->params.serverchan.url));
        safe_strcpy(ch->params.serverchan.sendkey, k1, sizeof(ch->params.serverchan.sendkey));
        break;
    case PUSH_TYPE_FEISHU:
        safe_strcpy(ch->params.feishu.url, url, sizeof(ch->params.feishu.url));
        safe_strcpy(ch->params.feishu.secret, k1, sizeof(ch->params.feishu.secret));
        break;
    case PUSH_TYPE_GOTIFY:
        safe_strcpy(ch->params.gotify.url, url, sizeof(ch->params.gotify.url));
        safe_strcpy(ch->params.gotify.token, k1, sizeof(ch->params.gotify.token));
        break;
    case PUSH_TYPE_TELEGRAM:
        safe_strcpy(ch->params.telegram.url, url, sizeof(ch->params.telegram.url));
        safe_strcpy(ch->params.telegram.chat_id, k1, sizeof(ch->params.telegram.chat_id));
        safe_strcpy(ch->params.telegram.bot_token, k2, sizeof(ch->params.telegram.bot_token));
        break;
    case PUSH_TYPE_SMTP:
        safe_strcpy(ch->params.smtp.server, url, sizeof(ch->params.smtp.server));
        safe_strcpy(ch->params.smtp.port, port, sizeof(ch->params.smtp.port));
        safe_strcpy(ch->params.smtp.user, k1, sizeof(ch->params.smtp.user));
        safe_strcpy(ch->params.smtp.password, k2, sizeof(ch->params.smtp.password));
        safe_strcpy(ch->params.smtp.to, body, sizeof(ch->params.smtp.to));
        break;
    case PUSH_TYPE_PUSHOVER:
        safe_strcpy(ch->params.pushover.url, url, sizeof(ch->params.pushover.url));
        safe_strcpy(ch->params.pushover.token, k1, sizeof(ch->params.pushover.token));
        safe_strcpy(ch->params.pushover.user, k2, sizeof(ch->params.pushover.user));
        break;
    default:
        break;
    }
}

app_config_t g_config;
bool g_config_valid = false;

schedule_task_t g_schedule_tasks[MAX_SCHEDULE_TASKS];

void config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS需要擦除后重新初始化");
        nvs_flash_erase();
        nvs_flash_init();
    }
    config_load();
    schedule_load();
    g_config_valid = config_is_valid();
}

static void nvs_get_str_safe(nvs_handle_t h, const char *key, char *buf, size_t buf_size, const char *def)
{
    size_t len = buf_size;
    if (nvs_get_str(h, key, buf, &len) != ESP_OK) {
        safe_strcpy(buf, def, buf_size);
    }
}

void config_load(void)
{
    nvs_handle_t h;
    if (nvs_open("sms_config", NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS命名空间不存在，使用默认配置");
        memset(&g_config, 0, sizeof(g_config));
        safe_strcpy(g_config.web_user, DEFAULT_WEB_USER, sizeof(g_config.web_user));
        safe_strcpy(g_config.web_pass, DEFAULT_WEB_PASS, sizeof(g_config.web_pass));
        // 默认不预创建任何推送通道，全部字段保持为空，由用户在前端自行新增与配置
        return;
    }

    nvs_get_str_safe(h, "webUser", g_config.web_user, sizeof(g_config.web_user), DEFAULT_WEB_USER);
    nvs_get_str_safe(h, "webPass", g_config.web_pass, sizeof(g_config.web_pass), DEFAULT_WEB_PASS);

    for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
        char key[32];
        push_channel_t *ch = &g_config.push_channels[i];
        memset(ch, 0, sizeof(push_channel_t));

        snprintf(key, sizeof(key), "push%dname", i);
        char def_name[32];
        snprintf(def_name, sizeof(def_name), "通道%d", i + 1);
        nvs_get_str_safe(h, key, ch->name, sizeof(ch->name), def_name);
        snprintf(key, sizeof(key), "push%dtype", i);
        uint8_t t = PUSH_TYPE_POST_JSON;
        nvs_get_u8(h, key, &t);
        ch->type = (push_type_t)t;

        snprintf(key, sizeof(key), "push%dparams", i);
        size_t blob_expected = 1 + sizeof(push_params_u);
        char *blob_buf = (char *)malloc(blob_expected);
        bool loaded = false;
        if (blob_buf) {
            size_t blob_len = blob_expected;
            if (nvs_get_blob(h, key, blob_buf, &blob_len) == ESP_OK && blob_len >= 1) {
                ch->enabled = (blob_buf[0] != 0);
                if (blob_len >= blob_expected)
                    memcpy(&ch->params, blob_buf + 1, sizeof(push_params_u));
                loaded = true;
            }
            free(blob_buf);
        }
        if (!loaded) {
            /* 迁移：旧格式多键 → 按 type 填入 union */
            snprintf(key, sizeof(key), "push%den", i);
            uint8_t en = 0;
            nvs_get_u8(h, key, &en);
            ch->enabled = (en != 0);
            char url[MAX_STR_LEN], k1[MAX_STR_LEN], k2[MAX_STR_LEN], body[MAX_BODY_LEN], port[16];
            snprintf(key, sizeof(key), "push%durl", i);
            nvs_get_str_safe(h, key, url, sizeof(url), "");
            snprintf(key, sizeof(key), "push%dk1", i);
            nvs_get_str_safe(h, key, k1, sizeof(k1), "");
            snprintf(key, sizeof(key), "push%dk2", i);
            nvs_get_str_safe(h, key, k2, sizeof(k2), "");
            snprintf(key, sizeof(key), "push%dbody", i);
            nvs_get_str_safe(h, key, body, sizeof(body), "");
            snprintf(key, sizeof(key), "push%dport", i);
            nvs_get_str_safe(h, key, port, sizeof(port), "465");
            push_migrate_legacy_to_union(ch, url, k1, k2, body, port);
        }
    }

    nvs_close(h);
    ESP_LOGI(TAG, "配置已加载");
}

bool config_get_wifi_sta(char *ssid, size_t ssid_size, char *pass, size_t pass_size)
{
    nvs_handle_t h;
    if (nvs_open("sms_config", NVS_READONLY, &h) != ESP_OK) return false;
    if (ssid_size > 0) ssid[0] = '\0';
    if (pass_size > 0) pass[0] = '\0';
    size_t len = ssid_size;
    if (nvs_get_str(h, "wifi_ssid", ssid, &len) != ESP_OK) { nvs_close(h); return false; }
    len = pass_size;
    if (nvs_get_str(h, "wifi_pass", pass, &len) != ESP_OK) { nvs_close(h); return false; }
    nvs_close(h);
    return strlen(ssid) > 0 && strlen(pass) > 0;
}

bool config_get_wifi_creds(char *ssid, size_t ssid_size, char *pass, size_t pass_size)
{
    nvs_handle_t h;
    if (nvs_open("sms_config", NVS_READONLY, &h) != ESP_OK) return false;
    if (ssid_size > 0) ssid[0] = '\0';
    if (pass_size > 0) pass[0] = '\0';
    size_t len = ssid_size;
    if (nvs_get_str(h, "wifi_ssid", ssid, &len) != ESP_OK) { nvs_close(h); return false; }
    nvs_get_str_safe(h, "wifi_pass", pass, pass_size, "");
    nvs_close(h);
    return true;
}

void config_set_wifi_sta(const char *ssid, const char *pass)
{
    if (!ssid || !pass) return;
    nvs_handle_t h;
    if (nvs_open("sms_config", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "wifi_ssid", ssid);
    nvs_set_str(h, "wifi_pass", pass);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi 凭证已保存");
}

void config_save(void)
{
    nvs_handle_t h;
    if (nvs_open("sms_config", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS打开失败");
        return;
    }

    nvs_set_str(h, "webUser", g_config.web_user);
    nvs_set_str(h, "webPass", g_config.web_pass);


    for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
        char key[32];
        size_t blob_size = 1 + sizeof(push_params_u);
        char *blob_buf = (char *)malloc(blob_size);
        const push_channel_t *ch = &g_config.push_channels[i];

        snprintf(key, sizeof(key), "push%dname", i);
        nvs_set_str(h, key, ch->name);
        snprintf(key, sizeof(key), "push%dtype", i);
        nvs_set_u8(h, key, (uint8_t)ch->type);
        if (blob_buf) {
            blob_buf[0] = ch->enabled ? 1 : 0;
            memcpy(blob_buf + 1, &ch->params, sizeof(push_params_u));
            snprintf(key, sizeof(key), "push%dparams", i);
            nvs_set_blob(h, key, blob_buf, blob_size);
            free(blob_buf);
        }
    }

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "配置已保存");
}

void schedule_load(void)
{
    nvs_handle_t h;
    memset(g_schedule_tasks, 0, sizeof(g_schedule_tasks));
    if (nvs_open("sms_config", NVS_READONLY, &h) != ESP_OK) return;

    for (int i = 0; i < MAX_SCHEDULE_TASKS; i++) {
        schedule_task_t *t = &g_schedule_tasks[i];
        char key[32];

        snprintf(key, sizeof(key), "sched%den", i);
        uint8_t en = 0;
        nvs_get_u8(h, key, &en);
        t->enabled = (en != 0);

        snprintf(key, sizeof(key), "sched%daction", i);
        uint8_t a = 0;
        nvs_get_u8(h, key, &a);
        t->action = (schedule_action_t)a;

        snprintf(key, sizeof(key), "sched%dkind", i);
        uint8_t k = 0;
        nvs_get_u8(h, key, &k);
        t->kind = (schedule_kind_t)k;

        snprintf(key, sizeof(key), "sched%dh", i);
        nvs_get_u8(h, key, &t->hour);
        snprintf(key, sizeof(key), "sched%dm", i);
        nvs_get_u8(h, key, &t->minute);
        snprintf(key, sizeof(key), "sched%dwday", i);
        nvs_get_u8(h, key, &t->weekday_mask);
        snprintf(key, sizeof(key), "sched%dmon", i);
        nvs_get_u8(h, key, &t->month_interval);
        if (t->month_interval == 0) t->month_interval = 1;
        snprintf(key, sizeof(key), "sched%dits", i);
        int64_t its = 0;
        nvs_get_i64(h, key, &its);
        t->initial_ts = (time_t)its;
        if (t->initial_ts == 0 && t->kind == SCHED_KIND_MONTH) {
            uint8_t im = 1, dom = 1;
            snprintf(key, sizeof(key), "sched%dim", i);
            nvs_get_u8(h, key, &im);
            if (im == 0) im = 1;
            snprintf(key, sizeof(key), "sched%ddom", i);
            nvs_get_u8(h, key, &dom);
            if (dom == 0) dom = 1;
            struct tm ref = {0};
            ref.tm_year = 100;
            ref.tm_mon = (int)im - 1;
            ref.tm_mday = (int)dom;
            ref.tm_hour = (int)t->hour;
            ref.tm_min = (int)t->minute;
            ref.tm_sec = 0;
            t->initial_ts = mktime(&ref);
            if (t->initial_ts == (time_t)-1) t->initial_ts = 0;
        }

        snprintf(key, sizeof(key), "sched%dphone", i);
        nvs_get_str_safe(h, key, t->phone, sizeof(t->phone), "");
        snprintf(key, sizeof(key), "sched%dbody", i);
        nvs_get_str_safe(h, key, t->message, sizeof(t->message), "");
        snprintf(key, sizeof(key), "sched%dhost", i);
        nvs_get_str_safe(h, key, t->ping_host, sizeof(t->ping_host), "8.8.8.8");

        snprintf(key, sizeof(key), "sched%dlast_ts", i);
        int64_t ts = 0;
        nvs_get_i64(h, key, &ts);
        t->last_run_ts = (time_t)ts;
        snprintf(key, sizeof(key), "sched%dlast_ok", i);
        uint8_t ok = 0;
        nvs_get_u8(h, key, &ok);
        t->last_ok = (ok != 0);
        snprintf(key, sizeof(key), "sched%dlast_msg", i);
        nvs_get_str_safe(h, key, t->last_msg, sizeof(t->last_msg), "");
    }
    nvs_close(h);
}

void schedule_save(void)
{
    nvs_handle_t h;
    if (nvs_open("sms_config", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS打开失败，无法保存定时任务");
        return;
    }
    for (int i = 0; i < MAX_SCHEDULE_TASKS; i++) {
        const schedule_task_t *t = &g_schedule_tasks[i];
        char key[32];

        snprintf(key, sizeof(key), "sched%den", i);
        nvs_set_u8(h, key, t->enabled ? 1 : 0);
        snprintf(key, sizeof(key), "sched%daction", i);
        nvs_set_u8(h, key, (uint8_t)t->action);
        snprintf(key, sizeof(key), "sched%dkind", i);
        nvs_set_u8(h, key, (uint8_t)t->kind);
        snprintf(key, sizeof(key), "sched%dh", i);
        nvs_set_u8(h, key, t->hour);
        snprintf(key, sizeof(key), "sched%dm", i);
        nvs_set_u8(h, key, t->minute);
        snprintf(key, sizeof(key), "sched%dwday", i);
        nvs_set_u8(h, key, t->weekday_mask);
        snprintf(key, sizeof(key), "sched%dmon", i);
        nvs_set_u8(h, key, t->month_interval);
        snprintf(key, sizeof(key), "sched%dits", i);
        nvs_set_i64(h, key, (int64_t)t->initial_ts);
        snprintf(key, sizeof(key), "sched%dphone", i);
        nvs_set_str(h, key, t->phone);
        snprintf(key, sizeof(key), "sched%dbody", i);
        nvs_set_str(h, key, t->message);
        snprintf(key, sizeof(key), "sched%dhost", i);
        nvs_set_str(h, key, t->ping_host);
        snprintf(key, sizeof(key), "sched%dlast_ts", i);
        nvs_set_i64(h, key, (int64_t)t->last_run_ts);
        snprintf(key, sizeof(key), "sched%dlast_ok", i);
        nvs_set_u8(h, key, t->last_ok ? 1 : 0);
        snprintf(key, sizeof(key), "sched%dlast_msg", i);
        nvs_set_str(h, key, t->last_msg);
    }
    nvs_commit(h);
    nvs_close(h);
}

bool config_is_push_channel_valid(const push_channel_t *ch)
{
    if (!ch->enabled) return false;
    switch (ch->type) {
    case PUSH_TYPE_POST_JSON:
        return strlen(ch->params.post_json.url) > 0 && strlen(ch->params.post_json.body) > 0;
    case PUSH_TYPE_BARK:
    case PUSH_TYPE_GET:
        return strlen(ch->params.bark.url) > 0;
    case PUSH_TYPE_DINGTALK:
        return strlen(ch->params.dingtalk.url) > 0;
    case PUSH_TYPE_PUSHPLUS:
        return strlen(ch->params.pushplus.token) > 0;
    case PUSH_TYPE_SERVERCHAN:
        return strlen(ch->params.serverchan.sendkey) > 0;
    case PUSH_TYPE_FEISHU:
        return strlen(ch->params.feishu.url) > 0;
    case PUSH_TYPE_GOTIFY:
        return strlen(ch->params.gotify.url) > 0 && strlen(ch->params.gotify.token) > 0;
    case PUSH_TYPE_TELEGRAM:
        return strlen(ch->params.telegram.chat_id) > 0 && strlen(ch->params.telegram.bot_token) > 0;
    case PUSH_TYPE_SMTP:
        return strlen(ch->params.smtp.server) > 0 && strlen(ch->params.smtp.user) > 0 && strlen(ch->params.smtp.to) > 0;
    case PUSH_TYPE_PUSHOVER:
        return strlen(ch->params.pushover.token) > 0 && strlen(ch->params.pushover.user) > 0;
    default:
        return false;
    }
}

bool config_is_valid(void)
{
    for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
        if (config_is_push_channel_valid(&g_config.push_channels[i])) {
            return true;
        }
    }
    return false;
}
