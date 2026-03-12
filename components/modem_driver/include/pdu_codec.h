#pragma once

#include <stdbool.h>
#include <stddef.h>

#define PDU_MAX_TEXT_LEN    512
#define PDU_MAX_SENDER_LEN  32
#define PDU_MAX_TS_LEN      32
#define PDU_MAX_HEX_LEN     600

// 编码错误码（参考 PDUlib）
#define PDU_ERR_OK                  0
#define PDU_ERR_ADDRESS_FORMAT     -1  // 号码格式错误或过长
#define PDU_ERR_GSM7_TOO_LONG      -2  // 7-bit 消息超过 160 字
#define PDU_ERR_UCS2_TOO_LONG      -3  // UCS-2 消息超过 70 字
#define PDU_ERR_BUFFER_TOO_SMALL   -4  // 内部缓冲区不足
#define PDU_ERR_NOT_GSM7           -5  // 含非 GSM 字符且无法用 UCS-2 单条发送

// PDU解码结果
typedef struct {
    char sender[PDU_MAX_SENDER_LEN];
    char timestamp[PDU_MAX_TS_LEN];
    char text[PDU_MAX_TEXT_LEN];
    int  concat_ref;
    int  concat_part;
    int  concat_total;
} pdu_decode_result_t;

// PDU编码结果
typedef struct {
    char hex[PDU_MAX_HEX_LEN];
    int  tpdu_len;  // TPDU 字节数，用于 AT+CMGS=<tpdu_len>
} pdu_encode_result_t;

// 解码 SMS-DELIVER PDU
bool pdu_decode(const char *hex_pdu, pdu_decode_result_t *result);

// 编码 SMS-SUBMIT PDU。成功返回 true，失败返回 false（可查 pdu_encode_last_error）
bool pdu_encode(const char *phone, const char *message, pdu_encode_result_t *result);
int pdu_encode_last_error(void);

// 检查字符串是否为有效十六进制
bool pdu_is_hex_string(const char *str);
