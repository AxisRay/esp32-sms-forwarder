#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/time.h>
#include <time.h>
#include "mbedtls/md.h"
#include "mbedtls/base64.h"

void safe_strcpy(char *dst, const char *src, size_t dst_size)
{
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

char *url_encode(const char *str)
{
    if (!str) return strdup("");
    size_t len = strlen(str);
    // 最坏情况每个字符编码为3字节
    char *enc = malloc(len * 3 + 1);
    if (!enc) return strdup("");
    char *p = enc;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c == ' ') {
            *p++ = '+';
        } else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            *p++ = c;
        } else {
            sprintf(p, "%%%02X", c);
            p += 3;
        }
    }
    *p = '\0';
    return enc;
}

char *json_escape(const char *str)
{
    if (!str) return strdup("");
    size_t len = strlen(str);
    // 最坏情况每个字符需要6字节(\uXXXX)
    char *esc = malloc(len * 6 + 1);
    if (!esc) return strdup("");
    char *p = esc;
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        switch (c) {
        case '"':  *p++ = '\\'; *p++ = '"'; break;
        case '\\': *p++ = '\\'; *p++ = '\\'; break;
        case '\n': *p++ = '\\'; *p++ = 'n'; break;
        case '\r': *p++ = '\\'; *p++ = 'r'; break;
        case '\t': *p++ = '\\'; *p++ = 't'; break;
        default:   *p++ = c; break;
        }
    }
    *p = '\0';
    return esc;
}

int64_t get_utc_millis(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0) {
        return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
    }
    return (int64_t)time(NULL) * 1000LL;
}

// 内部：HMAC-SHA256 + Base64 编码
static char *hmac_sha256_base64(const char *key, size_t key_len,
                                const char *data, size_t data_len)
{
    uint8_t hmac_result[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key, key_len);
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)data, data_len);
    mbedtls_md_hmac_finish(&ctx, hmac_result);
    mbedtls_md_free(&ctx);

    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, hmac_result, 32);
    char *b64 = malloc(b64_len + 1);
    if (!b64) return strdup("");
    mbedtls_base64_encode((unsigned char *)b64, b64_len + 1, &b64_len, hmac_result, 32);
    b64[b64_len] = '\0';
    return b64;
}

char *dingtalk_sign(const char *secret, int64_t timestamp)
{
    char ts_str[32];
    snprintf(ts_str, sizeof(ts_str), "%lld", (long long)timestamp);

    // stringToSign = timestamp + "\n" + secret
    size_t sign_len = strlen(ts_str) + 1 + strlen(secret) + 1;
    char *sign_str = malloc(sign_len);
    if (!sign_str) return strdup("");
    snprintf(sign_str, sign_len, "%s\n%s", ts_str, secret);

    char *b64 = hmac_sha256_base64(secret, strlen(secret), sign_str, strlen(sign_str));
    free(sign_str);

    char *encoded = url_encode(b64);
    free(b64);
    return encoded;
}

char *feishu_sign(const char *secret, int64_t timestamp)
{
    char ts_str[32];
    snprintf(ts_str, sizeof(ts_str), "%lld", (long long)timestamp);

    // stringToSign = timestamp + "\n" + secret
    size_t sign_len = strlen(ts_str) + 1 + strlen(secret) + 1;
    char *sign_str = malloc(sign_len);
    if (!sign_str) return strdup("");
    snprintf(sign_str, sign_len, "%s\n%s", ts_str, secret);

    char *b64 = hmac_sha256_base64(secret, strlen(secret), sign_str, strlen(sign_str));
    free(sign_str);
    return b64;
}

#include "app_events.h"
ESP_EVENT_DEFINE_BASE(APP_EVENT_BASE);

