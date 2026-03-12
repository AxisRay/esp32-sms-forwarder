#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MAX_CONCAT_PARTS      10
#define CONCAT_TIMEOUT_MS     30000
#define MAX_CONCAT_MESSAGES   5

#define SMS_SENDER_LEN        32
/* 长短信合并后可能达 2KB，历史条目需能存完整内容供界面展示 */
#define SMS_CONTENT_LEN       2048

// 初始化短信处理模块（长短信缓冲区等）
void sms_init(void);

// 检查UART上报的URC并处理（在主循环中调用）
void sms_check_urc(void);

// 检查长短信超时
void sms_check_concat_timeout(void);

// 发送短信（PDU模式）
bool sms_send(const char *phone, const char *message);

// 处理最终的短信内容（管理员命令检查和转发）
void sms_process_content(const char *sender, const char *text, const char *timestamp);

// 短信历史：内存环形缓冲，供 Web 历史接口读取（不再从 SIM 读列表）
typedef struct {
    int      id;
    int64_t  ts;       // 毫秒时间戳，便于前端显示
    char     sender[SMS_SENDER_LEN];
    char     content[SMS_CONTENT_LEN];
} sms_history_entry_t;
// 从环形缓冲区读取最近若干条（时间从新到旧），out 至少 max_count 项，返回实际条数
int sms_history_get(sms_history_entry_t *out, int max_count);
