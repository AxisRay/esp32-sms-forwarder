#include "smtp_client.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_tls.h"
#include <esp_timer.h>
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"

static const char *TAG = "smtp";

typedef struct {
    int port;
    bool use_tls;
} smtp_attempt_t;

// 读取SMTP响应
static int smtp_read_response(esp_tls_t *tls, char *buf, size_t buf_size, int timeout_ms)
{
    int pos = 0;
    int64_t start = esp_timer_get_time() / 1000;
    while (pos < (int)(buf_size - 1)) {
        int64_t elapsed = esp_timer_get_time() / 1000 - start;
        if (elapsed > timeout_ms) break;

        int ret = esp_tls_conn_read(tls, (unsigned char *)(buf + pos), 1);
        if (ret > 0) {
            pos += ret;
            buf[pos] = '\0';
            // SMTP响应以\r\n结尾
            if (pos >= 2 && buf[pos - 2] == '\r' && buf[pos - 1] == '\n') {
                // 多行响应：第4个字符为'-'时继续读
                if (pos < 4 || buf[pos - (pos > 4 ? 2 : pos) - 2] != '-') {
                    break;
                }
            }
        } else if (ret == 0) {
            break;
        } else if (ret != ESP_TLS_ERR_SSL_WANT_READ && ret != ESP_TLS_ERR_SSL_WANT_WRITE) {
            break;
        }
    }
    buf[pos] = '\0';
    return pos;
}

// 发送SMTP命令
static bool smtp_send_cmd(esp_tls_t *tls, const char *cmd, char *resp, size_t resp_size)
{
    size_t written = 0;
    size_t cmd_len = strlen(cmd);
    esp_err_t err = esp_tls_conn_write(tls, cmd, cmd_len);
    (void)written;
    if (err < 0) {
        ESP_LOGE(TAG, "SMTP写入失败");
        return false;
    }
    smtp_read_response(tls, resp, resp_size, 5000);
    ESP_LOGD(TAG, "SMTP响应: %s", resp);
    return true;
}

static esp_tls_t *smtp_connect(const char *server, int port, bool use_tls)
{
    esp_tls_cfg_t cfg = {
        .timeout_ms = 10000,
        .is_plain_tcp = !use_tls,
    };
    if (use_tls) cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_tls_t *tls = esp_tls_init();
    if (!tls) return NULL;

    int ret = esp_tls_conn_new_sync(server, strlen(server), port, &cfg, tls);
    if (ret != 1) {
        esp_tls_conn_destroy(tls);
        return NULL;
    }
    return tls;
}

static bool smtp_try_add_attempt(smtp_attempt_t *attempts, int *count, int port, bool use_tls)
{
    for (int i = 0; i < *count; i++) {
        if (attempts[i].port == port && attempts[i].use_tls == use_tls) return false;
    }
    attempts[*count].port = port;
    attempts[*count].use_tls = use_tls;
    (*count)++;
    return true;
}

static int smtp_build_attempts(const push_params_smtp_t *p, smtp_attempt_t *attempts, int max_attempts)
{
    int count = 0;
    int port = 0;
    if (p->port[0] != '\0') {
        port = atoi(p->port);
        if (port <= 0) port = 0;
    }

    if (port == 465) {
        smtp_try_add_attempt(attempts, &count, 465, true);
        smtp_try_add_attempt(attempts, &count, 587, false);
        smtp_try_add_attempt(attempts, &count, 25, false);
    } else if (port == 587) {
        smtp_try_add_attempt(attempts, &count, 587, false);
        smtp_try_add_attempt(attempts, &count, 465, true);
        smtp_try_add_attempt(attempts, &count, 25, false);
    } else if (port == 25) {
        smtp_try_add_attempt(attempts, &count, 25, false);
        smtp_try_add_attempt(attempts, &count, 465, true);
        smtp_try_add_attempt(attempts, &count, 587, false);
    } else if (port > 0) {
        smtp_try_add_attempt(attempts, &count, port, true);
        smtp_try_add_attempt(attempts, &count, port, false);
        smtp_try_add_attempt(attempts, &count, 465, true);
        smtp_try_add_attempt(attempts, &count, 587, false);
        smtp_try_add_attempt(attempts, &count, 25, false);
    } else {
        smtp_try_add_attempt(attempts, &count, 465, true);
        smtp_try_add_attempt(attempts, &count, 587, false);
        smtp_try_add_attempt(attempts, &count, 25, false);
    }

    return count > max_attempts ? max_attempts : count;
}

static bool smtp_send_email_on_connection(esp_tls_t *tls, const push_params_smtp_t *p, const char *subject, const char *body)
{
    char *resp = malloc(512);
    if (!resp) {
        ESP_LOGE(TAG, "内存不足");
        return false;
    }
    bool ok = true;

    smtp_read_response(tls, resp, 512, 5000);
    ESP_LOGD(TAG, "欢迎: %s", resp);

    char ehlo[128];
    snprintf(ehlo, sizeof(ehlo), "EHLO esp32\r\n");
    ok = smtp_send_cmd(tls, ehlo, resp, 512);
    if (!ok || resp[0] != '2') {
        ESP_LOGE(TAG, "EHLO失败: %s", resp);
        goto cleanup;
    }

    ok = smtp_send_cmd(tls, "AUTH LOGIN\r\n", resp, 512);
    if (!ok || resp[0] != '3') {
        ESP_LOGE(TAG, "AUTH LOGIN失败: %s", resp);
        goto cleanup;
    }

    {
        size_t b64_len = 0;
        mbedtls_base64_encode(NULL, 0, &b64_len, (const unsigned char *)p->user, strlen(p->user));
        char *b64_user = malloc(b64_len + 4);
        if (!b64_user) goto cleanup;
        mbedtls_base64_encode((unsigned char *)b64_user, b64_len + 1, &b64_len, (const unsigned char *)p->user, strlen(p->user));
        strcat(b64_user, "\r\n");
        ok = smtp_send_cmd(tls, b64_user, resp, 512);
        free(b64_user);
        if (!ok || resp[0] != '3') {
            ESP_LOGE(TAG, "用户名认证失败: %s", resp);
            goto cleanup;
        }
    }

    {
        size_t b64_len = 0;
        mbedtls_base64_encode(NULL, 0, &b64_len, (const unsigned char *)p->password, strlen(p->password));
        char *b64_pass = malloc(b64_len + 4);
        if (!b64_pass) goto cleanup;
        mbedtls_base64_encode((unsigned char *)b64_pass, b64_len + 1, &b64_len, (const unsigned char *)p->password, strlen(p->password));
        strcat(b64_pass, "\r\n");
        ok = smtp_send_cmd(tls, b64_pass, resp, 512);
        free(b64_pass);
        if (!ok || resp[0] != '2') {
            ESP_LOGE(TAG, "密码认证失败: %s", resp);
            goto cleanup;
        }
    }

    {
        char cmd[320];
        snprintf(cmd, sizeof(cmd), "MAIL FROM:<%s>\r\n", p->user);
        ok = smtp_send_cmd(tls, cmd, resp, 512);
        if (!ok || resp[0] != '2') {
            ESP_LOGE(TAG, "MAIL FROM失败: %s", resp);
            goto cleanup;
        }
    }

    {
        char cmd[320];
        snprintf(cmd, sizeof(cmd), "RCPT TO:<%s>\r\n", p->to);
        ok = smtp_send_cmd(tls, cmd, resp, 512);
        if (!ok || resp[0] != '2') {
            ESP_LOGE(TAG, "RCPT TO失败: %s", resp);
            goto cleanup;
        }
    }

    ok = smtp_send_cmd(tls, "DATA\r\n", resp, 512);
    if (!ok || resp[0] != '3') {
        ESP_LOGE(TAG, "DATA失败: %s", resp);
        goto cleanup;
    }

    {
        char *mail = NULL;
        time_t now = time(NULL);
        struct tm tm_info;
        gmtime_r(&now, &tm_info);
        char date_str[64];
        strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S +0000", &tm_info);

        int r = asprintf(&mail,
            "From: sms notify <%s>\r\n"
            "To: <%s>\r\n"
            "Subject: =?UTF-8?B?",
            p->user, p->to);
        if (r < 0 || !mail) goto cleanup;

        size_t subj_b64_len = 0;
        mbedtls_base64_encode(NULL, 0, &subj_b64_len, (const unsigned char *)subject, strlen(subject));
        char *subj_b64 = malloc(subj_b64_len + 1);
        if (subj_b64) {
            mbedtls_base64_encode((unsigned char *)subj_b64, subj_b64_len + 1, &subj_b64_len, (const unsigned char *)subject, strlen(subject));
            char *header = NULL;
            int r2 = asprintf(&header,
                "%s%s?=\r\n"
                "Date: %s\r\n"
                "MIME-Version: 1.0\r\n"
                "Content-Type: text/plain; charset=UTF-8\r\n"
                "Content-Transfer-Encoding: 8bit\r\n"
                "\r\n"
                "%s\r\n"
                ".\r\n",
                mail, subj_b64, date_str, body);
            free(subj_b64);
            free(mail);
            mail = (r2 >= 0 && header) ? header : NULL;
        } else {
            free(mail);
            mail = NULL;
        }

        if (mail) {
            ok = smtp_send_cmd(tls, mail, resp, 512);
            free(mail);
            if (!ok || resp[0] != '2') {
                ESP_LOGE(TAG, "邮件发送失败: %s", resp);
                goto cleanup;
            }
        }
    }

    smtp_send_cmd(tls, "QUIT\r\n", resp, 512);
    ESP_LOGI(TAG, "邮件发送完成");

cleanup:
    free(resp);
    return ok;
}

void smtp_send_email_with_params(const push_params_smtp_t *p, const char *subject, const char *body)
{
    if (!p || strlen(p->server) == 0 || strlen(p->user) == 0 ||
        strlen(p->password) == 0 || strlen(p->to) == 0) {
        ESP_LOGW(TAG, "邮件配置不完整，跳过发送");
        return;
    }

    ESP_LOGI(TAG, "准备发送邮件到 %s", p->to);

    smtp_attempt_t attempts[5] = {0};
    int attempt_count = smtp_build_attempts(p, attempts, 5);
    for (int i = 0; i < attempt_count; i++) {
        const smtp_attempt_t *attempt = &attempts[i];
        ESP_LOGI(TAG, "尝试SMTP连接: %s:%d (%s)", p->server, attempt->port, attempt->use_tls ? "TLS" : "PLAIN");
        esp_tls_t *tls = smtp_connect(p->server, attempt->port, attempt->use_tls);
        if (!tls) {
            ESP_LOGW(TAG, "SMTP连接失败: %s:%d (%s)", p->server, attempt->port, attempt->use_tls ? "TLS" : "PLAIN");
            continue;
        }
        bool ok = smtp_send_email_on_connection(tls, p, subject, body);
        esp_tls_conn_destroy(tls);
        if (ok) return;
    }

    ESP_LOGE(TAG, "SMTP发送失败，所有自动尝试均未成功");
}