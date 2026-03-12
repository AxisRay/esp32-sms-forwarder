#pragma once

#include <stdint.h>
#include <stddef.h>

// URL编码，调用者需free返回值
char *url_encode(const char *str);

// JSON字符串转义，调用者需free返回值
char *json_escape(const char *str);

// 获取UTC毫秒时间戳（用于钉钉签名等）
int64_t get_utc_millis(void);

// 钉钉 HMAC-SHA256 签名，调用者需free返回值
char *dingtalk_sign(const char *secret, int64_t timestamp);

// 飞书 HMAC-SHA256 签名，调用者需free返回值
char *feishu_sign(const char *secret, int64_t timestamp);

// 安全字符串拷贝到固定长度缓冲区
void safe_strcpy(char *dst, const char *src, size_t dst_size);
