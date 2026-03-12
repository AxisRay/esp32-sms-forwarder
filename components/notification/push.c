#include "push.h"
#include "smtp_client.h"
#include "utils.h"
#include "wifi_mgr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include <cJSON.h>

static const char *TAG = "push";

// HTTP响应缓冲区大小
#define HTTP_RESP_BUF_SIZE 512

// 占位符替换：将模板中的 {sender} {message} {timestamp} 替换为实际值，写入 out（最多 out_size-1 字符）
// 零堆分配、单遍扫描，支持同一占位符多次出现
static bool fill_body_from_template(const char *tpl, const char *sender_esc,
                                    const char *msg_esc, const char *ts_esc,
                                    char *out, size_t out_size)
{
    if (!tpl || !out || out_size == 0) return false;

    const struct { const char *key; size_t key_len; const char *val; } reps[] = {
        { "{sender}",    8, sender_esc },
        { "{message}",   9, msg_esc },
        { "{timestamp}", 11, ts_esc },
    };

    size_t si = 0, di = 0;
    size_t tpl_len = strlen(tpl);

    while (si < tpl_len && di < out_size - 1) {
        bool replaced = false;
        for (int r = 0; r < 3; r++) {
            if (tpl_len - si >= reps[r].key_len &&
                memcmp(tpl + si, reps[r].key, reps[r].key_len) == 0) {
                const char *val = reps[r].val ? reps[r].val : "";
                size_t val_len = strlen(val);
                size_t copy_len = (val_len < out_size - 1 - di) ? val_len : out_size - 1 - di;
                memcpy(out + di, val, copy_len);
                di += copy_len;
                si += reps[r].key_len;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            out[di++] = tpl[si++];
        }
    }
    out[di] = '\0';
    return true;
}

// 执行HTTP POST请求
static int http_post(const char *url, const char *content_type,
                     const char *post_data)
{
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) return -1;

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status = -1;
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
    } else {
        ESP_LOGE(TAG, "HTTP请求失败: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return status;
}

// 执行HTTP GET请求
static int http_get(const char *url)
{
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) return -1;

    esp_err_t err = esp_http_client_perform(client);
    int status = -1;
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    return status;
}

void push_send_to_channel(const push_channel_t *ch,
                           const char *sender, const char *message,
                           const char *timestamp)
{
    if (!ch->enabled) return;

    const char *ch_name = strlen(ch->name) > 0 ? ch->name : "未命名通道";
    ESP_LOGI(TAG, "发送到推送通道: %s (类型=%d)", ch_name, ch->type);

    char *sender_esc  = json_escape(sender);
    char *msg_esc     = json_escape(message);
    char *ts_esc      = json_escape(timestamp);
    int status = -1;

    switch (ch->type) {
    case PUSH_TYPE_POST_JSON: {
        const push_params_post_json_t *p = &ch->params.post_json;
        char *body = NULL;
        if (p->body[0] != '\0') {
            char body_buf[MAX_BODY_LEN * 2];
            if (fill_body_from_template(p->body, sender_esc, msg_esc, ts_esc,
                                        body_buf, sizeof(body_buf))) {
                status = http_post(p->url, "application/json", body_buf);
            }
        } else {
            asprintf(&body, "{\"sender\":\"%s\",\"message\":\"%s\",\"timestamp\":\"%s\"}",
                     sender_esc, msg_esc, ts_esc);
            if (body) {
                status = http_post(p->url, "application/json", body);
                free(body);
            }
        }
        break;
    }
    case PUSH_TYPE_BARK: {
        const push_params_bark_t *p = &ch->params.bark;
        char *body = NULL;
        asprintf(&body, "{\"title\":\"%s\",\"body\":\"%s\"}", sender_esc, msg_esc);
        if (body) {
            status = http_post(p->url, "application/json", body);
            free(body);
        }
        break;
    }
    case PUSH_TYPE_GET: {
        const push_params_get_t *p = &ch->params.get;
        char *s_enc = url_encode(sender);
        char *m_enc = url_encode(message);
        char *t_enc = url_encode(timestamp);
        char *url = NULL;
        char sep = strchr(p->url, '?') ? '&' : '?';
        asprintf(&url, "%s%csender=%s&message=%s&timestamp=%s",
                 p->url, sep, s_enc, m_enc, t_enc);
        if (url) {
            status = http_get(url);
            free(url);
        }
        free(s_enc); free(m_enc); free(t_enc);
        break;
    }
    case PUSH_TYPE_DINGTALK: {
        const push_params_dingtalk_t *p = &ch->params.dingtalk;
        char *webhook = strdup(p->url);
        if (!webhook) break;
        if (strlen(p->secret) > 0) {
            int64_t ts = get_utc_millis();
            char *sign = dingtalk_sign(p->secret, ts);
            char sep = strchr(webhook, '?') ? '&' : '?';
            char *tmp = NULL;
            asprintf(&tmp, "%s%ctimestamp=%lld&sign=%s", webhook, sep, (long long)ts, sign);
            free(webhook);
            webhook = tmp;
            free(sign);
        }
        char *body = NULL;
        asprintf(&body,
            "{\"msgtype\":\"text\",\"text\":{\"content\":"
            "\"📱短信通知\\n发送者: %s\\n内容: %s\\n时间: %s\"}}",
            sender_esc, msg_esc, ts_esc);
        if (body) {
            status = http_post(webhook, "application/json", body);
            free(body);
        }
        free(webhook);
        break;
    }
    case PUSH_TYPE_PUSHPLUS: {
        const push_params_pushplus_t *p = &ch->params.pushplus;
        const char *push_url = strlen(p->url) > 0 ? p->url : "http://www.pushplus.plus/send";
        const char *channel_val = "wechat";
        if (strlen(p->channel) > 0 &&
            (strcmp(p->channel, "wechat") == 0 ||
             strcmp(p->channel, "extension") == 0 ||
             strcmp(p->channel, "app") == 0)) {
            channel_val = p->channel;
        }
        char *body = NULL;
        asprintf(&body,
            "{\"token\":\"%s\",\"title\":\"短信来自: %s\","
            "\"content\":\"<b>发送者:</b> %s<br><b>时间:</b> %s<br><b>内容:</b><br>%s\","
            "\"channel\":\"%s\"}",
            p->token, sender_esc, sender_esc, ts_esc, msg_esc, channel_val);
        if (body) {
            status = http_post(push_url, "application/json", body);
            free(body);
        }
        break;
    }
    case PUSH_TYPE_SERVERCHAN: {
        const push_params_serverchan_t *p = &ch->params.serverchan;
        char sc_url[512];
        if (strlen(p->url) > 0) {
            strncpy(sc_url, p->url, sizeof(sc_url) - 1);
        } else {
            snprintf(sc_url, sizeof(sc_url), "https://sctapi.ftqq.com/%s.send", p->sendkey);
        }
        sc_url[sizeof(sc_url) - 1] = '\0';
        char *title_enc = url_encode(sender);
        char desp_raw[1024];
        snprintf(desp_raw, sizeof(desp_raw),
                 "**发送者:** %s\n\n**时间:** %s\n\n**内容:**\n\n%s",
                 sender, timestamp, message);
        char *desp_enc = url_encode(desp_raw);
        char *body = NULL;
        asprintf(&body, "title=%s&desp=%s", title_enc, desp_enc);
        if (body) {
            status = http_post(sc_url, "application/x-www-form-urlencoded", body);
            free(body);
        }
        free(title_enc);
        free(desp_enc);
        break;
    }
    case PUSH_TYPE_FEISHU: {
        const push_params_feishu_t *p = &ch->params.feishu;
        char json_buf[1024];
        int offset = 0;
        if (strlen(p->secret) > 0) {
            int64_t ts = time(NULL);
            char *sign = feishu_sign(p->secret, ts);
            offset = snprintf(json_buf, sizeof(json_buf),
                "{\"timestamp\":\"%lld\",\"sign\":\"%s\","
                "\"msg_type\":\"text\",\"content\":{\"text\":"
                "\"📱短信通知\\n发送者: %s\\n内容: %s\\n时间: %s\"}}",
                (long long)ts, sign, sender_esc, msg_esc, ts_esc);
            free(sign);
        } else {
            offset = snprintf(json_buf, sizeof(json_buf),
                "{\"msg_type\":\"text\",\"content\":{\"text\":"
                "\"📱短信通知\\n发送者: %s\\n内容: %s\\n时间: %s\"}}",
                sender_esc, msg_esc, ts_esc);
        }
        (void)offset;
        status = http_post(p->url, "application/json", json_buf);
        break;
    }
    case PUSH_TYPE_GOTIFY: {
        const push_params_gotify_t *p = &ch->params.gotify;
        char *gotify_url = NULL;
        size_t ul = strlen(p->url);
        asprintf(&gotify_url, "%s%smessage?token=%s",
                 p->url, (ul > 0 && p->url[ul - 1] == '/') ? "" : "/", p->token);
        if (!gotify_url) break;
        char *body = NULL;
        asprintf(&body,
            "{\"title\":\"短信来自: %s\",\"message\":\"%s\\n\\n时间: %s\",\"priority\":5}",
            sender_esc, msg_esc, ts_esc);
        if (body) {
            status = http_post(gotify_url, "application/json", body);
            free(body);
        }
        free(gotify_url);
        break;
    }
    case PUSH_TYPE_TELEGRAM: {
        const push_params_telegram_t *p = &ch->params.telegram;
        const char *base = strlen(p->url) > 0 ? p->url : "https://api.telegram.org";
        char *tg_url = NULL;
        asprintf(&tg_url, "%s/bot%s/sendMessage", base, p->bot_token);
        if (!tg_url) break;
        char *body = NULL;
        asprintf(&body,
            "{\"chat_id\":\"%s\",\"text\":"
            "\"📱短信通知\\n发送者: %s\\n内容: %s\\n时间: %s\"}",
            p->chat_id, sender_esc, msg_esc, ts_esc);
        if (body) {
            status = http_post(tg_url, "application/json", body);
            free(body);
        }
        free(tg_url);
        break;
    }
    case PUSH_TYPE_PUSHOVER: {
        const push_params_pushover_t *p = &ch->params.pushover;
        const char *po_url = strlen(p->url) > 0 ? p->url : "https://api.pushover.net/1/messages.json";
        char *token_enc = url_encode(p->token);
        char *user_enc  = url_encode(p->user);
        char *msg_enc_po = url_encode(message);
        char *title_enc = url_encode(sender);
        char *body = NULL;
        asprintf(&body, "token=%s&user=%s&message=%s&title=%s",
                 token_enc, user_enc, msg_enc_po, title_enc);
        if (body) {
            status = http_post(po_url, "application/x-www-form-urlencoded", body);
            free(body);
        }
        free(token_enc);
        free(user_enc);
        free(msg_enc_po);
        free(title_enc);
        break;
    }
    case PUSH_TYPE_SMTP: {
        const push_params_smtp_t *p = &ch->params.smtp;
        char subj[256];
        snprintf(subj, sizeof(subj), "短信%s,%.200s", sender, message);
        char body_buf[1024];
        snprintf(body_buf, sizeof(body_buf), "来自：%s，时间：%s，内容：%s",
                 sender, timestamp, message);
        smtp_send_email_with_params(p, subj, body_buf);
        status = 0;
        break;
    }
    default:
        ESP_LOGW(TAG, "未知推送类型: %d", ch->type);
        break;
    }

    free(sender_esc);
    free(msg_esc);
    free(ts_esc);

    if (status >= 0) {
        ESP_LOGI(TAG, "[%s] 响应码: %d", ch_name, status);
    }
}

void push_send_to_all(const char *sender, const char *message, const char *timestamp)
{
    ESP_LOGI(TAG, "push_send_to_all 进入 sender=%s", sender ? sender : "(null)");
    if (!wifi_mgr_is_connected()) {
        ESP_LOGW(TAG, "WiFi未连接，跳过推送");
        return;
    }

    bool has_any = false;
    for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
        if (config_is_push_channel_valid(&g_config.push_channels[i])) {
            has_any = true;
            break;
        }
    }
    if (!has_any) {
        ESP_LOGI(TAG, "没有启用的推送通道，跳过推送");
        return;
    }

    ESP_LOGI(TAG, "=== 开始多通道推送 ===");
    for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
        if (config_is_push_channel_valid(&g_config.push_channels[i])) {
            push_send_to_channel(&g_config.push_channels[i], sender, message, timestamp);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    ESP_LOGI(TAG, "=== 多通道推送完成 ===");
}