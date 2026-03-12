#include "pdu_codec.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"

static const char *TAG = "pdu";

/* ========== 辅助函数 ========== */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static uint8_t hex_byte(const char *s)
{
    return (uint8_t)((hex_val(s[0]) << 4) | hex_val(s[1]));
}

static void byte_to_hex(uint8_t b, char *out)
{
    static const char hex_chars[] = "0123456789ABCDEF";
    out[0] = hex_chars[(b >> 4) & 0x0F];
    out[1] = hex_chars[b & 0x0F];
}

bool pdu_is_hex_string(const char *str)
{
    if (!str || !*str) return false;
    for (const char *p = str; *p; p++) {
        if (!isxdigit((unsigned char)*p)) return false;
    }
    return true;
}

// 将semi-octet编码的号码解码
static void decode_address(const char *hex, int num_digits, char *out, size_t out_size)
{
    int pos = 0;
    int hex_idx = 0;
    for (int i = 0; i < num_digits && pos < (int)(out_size - 1); i += 2) {
        char lo = hex[hex_idx + 1];
        char hi = hex[hex_idx];
        if (lo != 'F' && lo != 'f') {
            out[pos++] = lo;
        }
        if (hi != 'F' && hi != 'f' && (i + 1) < num_digits) {
            out[pos++] = hi;
        }
        hex_idx += 2;
    }
    out[pos] = '\0';
}

// 解码BCD时间戳 -> "YY/MM/DD HH:MM:SS"
static void decode_timestamp(const char *hex, char *out, size_t out_size)
{
    // SCTS: 7字节 BCD编码 YY MM DD HH MM SS TZ (semi-octet swap)
    char parts[7][3];
    for (int i = 0; i < 7; i++) {
        parts[i][0] = hex[i * 2 + 1];
        parts[i][1] = hex[i * 2];
        parts[i][2] = '\0';
    }
    snprintf(out, out_size, "%s/%s/%s %s:%s:%s",
             parts[0], parts[1], parts[2],
             parts[3], parts[4], parts[5]);
}

// UTF-16BE (UCS2) -> UTF-8
static int ucs2_to_utf8(const uint8_t *ucs2, int ucs2_len, char *utf8, int utf8_size)
{
    int pos = 0;
    for (int i = 0; i + 1 < ucs2_len; i += 2) {
        uint16_t code = ((uint16_t)ucs2[i] << 8) | ucs2[i + 1];
        if (code < 0x80) {
            if (pos + 1 >= utf8_size) break;
            utf8[pos++] = (char)code;
        } else if (code < 0x800) {
            if (pos + 2 >= utf8_size) break;
            utf8[pos++] = (char)(0xC0 | (code >> 6));
            utf8[pos++] = (char)(0x80 | (code & 0x3F));
        } else {
            if (pos + 3 >= utf8_size) break;
            utf8[pos++] = (char)(0xE0 | (code >> 12));
            utf8[pos++] = (char)(0x80 | ((code >> 6) & 0x3F));
            utf8[pos++] = (char)(0x80 | (code & 0x3F));
        }
    }
    utf8[pos] = '\0';
    return pos;
}

// GSM 7-bit默认字母表 -> ASCII（简化版本）
static const char gsm7_basic[128] = {
    '@', '\xa3', '$', '\xa5', '\xe8', '\xe9', '\xf9', '\xec',
    '\xf2', '\xc7', '\n', '\xd8', '\xf8', '\r', '\xc5', '\xe5',
    0x94, '_', 0x86, 0x93, 0x9c, 0x9e, 0xa3, 0xaa,
    0xab, 0xac, ' ', 0xae, 0xaf, 0xb0, 0xb1, 0xbf,
    ' ', '!', '"', '#', 0xa4, '%', '&', '\'',
    '(', ')', '*', '+', ',', '-', '.', '/',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', ':', ';', '<', '=', '>', '?',
    0xa1, 'A', 'B', 'C', 'D', 'E', 'F', 'G',
    'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
    'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W',
    'X', 'Y', 'Z', 0xc4, 0xd6, 0xd1, 0xdc, 0xa7,
    0xbf, 'a', 'b', 'c', 'd', 'e', 'f', 'g',
    'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
    'p', 'q', 'r', 's', 't', 'u', 'v', 'w',
    'x', 'y', 'z', 0xe4, 0xf6, 0xf1, 0xfc, 0xe0,
};

// 解码GSM 7-bit编码数据
static int decode_gsm7(const uint8_t *data, int data_len, int num_chars,
                       int fill_bits, char *out, int out_size)
{
    int pos = 0;
    int bit_pos = fill_bits;
    for (int i = 0; i < num_chars && pos < out_size - 1; i++) {
        int byte_idx = bit_pos / 8;
        int bit_offset = bit_pos % 8;
        uint8_t val;
        if (byte_idx < data_len) {
            val = (data[byte_idx] >> bit_offset) & 0x7F;
            if (bit_offset > 1 && (byte_idx + 1) < data_len) {
                val |= (data[byte_idx + 1] << (8 - bit_offset)) & 0x7F;
            }
        } else {
            break;
        }
        out[pos++] = (val < 128) ? gsm7_basic[val] : '?';
        bit_pos += 7;
    }
    out[pos] = '\0';
    return pos;
}

/* ========== PDU解码 ========== */

bool pdu_decode(const char *hex_pdu, pdu_decode_result_t *result)
{
    if (!hex_pdu || !result) return false;
    memset(result, 0, sizeof(*result));

    int hex_len = strlen(hex_pdu);
    if (hex_len < 20) {
        ESP_LOGE(TAG, "PDU数据太短: %d", hex_len);
        return false;
    }

    int idx = 0; // 当前十六进制字符索引

    // 1. SCA（服务中心地址）
    int sca_len = hex_byte(hex_pdu + idx);
    idx += 2;
    idx += sca_len * 2; // 跳过SCA内容

    // 2. PDU类型
    uint8_t pdu_type = hex_byte(hex_pdu + idx);
    idx += 2;
    bool has_udhi = (pdu_type & 0x40) != 0; // TP-UDHI位

    // 3. OA（发送者地址）
    int oa_len = hex_byte(hex_pdu + idx); // 号码位数
    idx += 2;
    uint8_t oa_type = hex_byte(hex_pdu + idx);
    idx += 2;

    int oa_hex_len = (oa_len + 1) & ~1; // 对齐到偶数
    if (oa_type == 0x91) {
        result->sender[0] = '+';
        decode_address(hex_pdu + idx, oa_hex_len, result->sender + 1,
                       sizeof(result->sender) - 1);
    } else {
        decode_address(hex_pdu + idx, oa_hex_len, result->sender, sizeof(result->sender));
    }
    idx += oa_hex_len;

    // 4. PID
    idx += 2;

    // 5. DCS（数据编码方案）
    uint8_t dcs = hex_byte(hex_pdu + idx);
    idx += 2;

    // 6. SCTS（时间戳，7字节）
    if (idx + 14 <= hex_len) {
        decode_timestamp(hex_pdu + idx, result->timestamp, sizeof(result->timestamp));
    }
    idx += 14;

    // 7. UDL（用户数据长度）+ 8. UD（用户数据）：若 PDU 到 SCTS 结束（如 28 字节），则无 UD
    if (idx + 2 > hex_len) {
        result->text[0] = '\0';
        return true;
    }
    int udl = hex_byte(hex_pdu + idx);
    idx += 2;

    // 8. UD（用户数据）
    int ud_start = idx;
    int ud_hex_len = hex_len - idx;
    if (ud_hex_len < 0) ud_hex_len = 0;
    int ud_byte_len = ud_hex_len / 2;

    // 将UD十六进制转为字节数组（可能为 0 长度）
    uint8_t *ud_bytes = malloc(ud_byte_len + 1);
    if (!ud_bytes) return false;
    for (int i = 0; i < ud_byte_len; i++) {
        ud_bytes[i] = hex_byte(hex_pdu + ud_start + i * 2);
    }

    int data_offset = 0; // UDH之后数据的起始偏移
    int fill_bits = 0;

    // 解析UDH（User Data Header）
    if (has_udhi && ud_byte_len > 0) {
        int udhl = ud_bytes[0]; // UDH长度
        int uh_idx = 1;
        while (uh_idx < udhl + 1 && uh_idx < ud_byte_len) {
            uint8_t iei = ud_bytes[uh_idx++];
            if (uh_idx >= ud_byte_len) break;
            uint8_t ie_len = ud_bytes[uh_idx++];

            if (iei == 0x00 && ie_len == 3 && uh_idx + 3 <= ud_byte_len) {
                // 8-bit reference concatenated SMS
                result->concat_ref   = ud_bytes[uh_idx];
                result->concat_total = ud_bytes[uh_idx + 1];
                result->concat_part  = ud_bytes[uh_idx + 2];
            } else if (iei == 0x08 && ie_len == 4 && uh_idx + 4 <= ud_byte_len) {
                // 16-bit reference concatenated SMS
                result->concat_ref   = (ud_bytes[uh_idx] << 8) | ud_bytes[uh_idx + 1];
                result->concat_total = ud_bytes[uh_idx + 2];
                result->concat_part  = ud_bytes[uh_idx + 3];
            }
            uh_idx += ie_len;
        }
        data_offset = udhl + 1;

        // GSM 7-bit编码时需要补齐到septet边界
        if ((dcs & 0x0C) == 0x00) {
            fill_bits = ((data_offset * 8) + 6) / 7 * 7 - data_offset * 8;
            if (fill_bits < 0) fill_bits = 0;
        }
    }

    // 解码用户数据
    bool is_ucs2 = false;
    if ((dcs & 0x0C) == 0x08) {
        is_ucs2 = true; // UCS2编码
    }

    if (is_ucs2) {
        ucs2_to_utf8(ud_bytes + data_offset, ud_byte_len - data_offset,
                     result->text, sizeof(result->text));
    } else {
        // GSM 7-bit编码
        int num_chars = udl;
        if (has_udhi) {
            int udhl = ud_bytes[0];
            int header_septets = ((udhl + 1) * 8 + 6) / 7;
            num_chars = udl - header_septets;
        }
        decode_gsm7(ud_bytes + data_offset, ud_byte_len - data_offset,
                    num_chars, fill_bits, result->text, sizeof(result->text));
    }

    free(ud_bytes);

    ESP_LOGI(TAG, "PDU解码成功: sender=%s, text_len=%d, concat=%d/%d, content: %s",
             result->sender, (int)strlen(result->text),
             result->concat_part, result->concat_total, 
             result->text);
    return true;
}

/* ========== PDU编码（参考 PDUlib / 3GPP TS 23.038, 23.040）========== */

static int s_last_error = PDU_ERR_OK;

int pdu_encode_last_error(void)
{
    return s_last_error;
}

// 校验并规范化号码：仅数字与可选前导 +，去除空格
static bool normalize_phone(const char *phone, char *out, size_t out_size)
{
    if (!phone || !out || out_size < 2) return false;
    const char *p = phone;
    size_t i = 0;
    if (*p == '+') { out[i++] = '+'; p++; }
    for (; *p && i < out_size - 1; p++) {
        if (*p >= '0' && *p <= '9')
            out[i++] = *p;
        else if (*p != ' ' && *p != '\t' && *p != '-')
            return false;  // 非法字符
    }
    out[i] = '\0';
    if (i == 0 || (out[0] == '+' && i < 2)) return false;
    size_t digits = (out[0] == '+') ? i - 1 : i;
    if (digits > 15) return false;  // 国际最长 15 位
    return true;
}

// UTF-8 -> UCS-2 (Big Endian as per 3GPP)
static int utf8_to_ucs2(const char *utf8, uint8_t *ucs2, int ucs2_size)
{
    int pos = 0;
    const uint8_t *s = (const uint8_t *)utf8;
    while (*s && pos + 1 < ucs2_size) {
        uint16_t code;
        if (*s < 0x80) {
            code = *s++;
        } else if ((*s & 0xE0) == 0xC0) {
            code = (*s++ & 0x1F) << 6;
            if (*s) code |= (*s++ & 0x3F);
        } else if ((*s & 0xF0) == 0xE0) {
            code = (*s++ & 0x0F) << 12;
            if (*s) { code |= (*s++ & 0x3F) << 6; }
            if (*s) { code |= (*s++ & 0x3F); }
        } else {
            s++;
            code = '?';
        }
        // 恢复为 Big Endian (MSB在前)
        ucs2[pos++] = (uint8_t)(code >> 8);
        ucs2[pos++] = (uint8_t)(code & 0xFF);
    }
    return pos;
}

// GSM 03.38 基本集： septet -> 字符（解码用，已有）
// 编码用反向表：字符码 -> septet，255 表示不在基本集
static uint8_t s_gsm7_reverse[256];

// 扩展表：字符码 -> 0x1B 后的 septet；0 表示不在扩展集
static const uint8_t gsm7_extended[256] = {
    [0x0C] = 0x0A,  // \f
    [0x5E] = 0x14,  // ^
    [0x7B] = 0x28,  // {
    [0x7D] = 0x29,  // }
    [0x5C] = 0x2F,  /* 反斜杠 */
    [0x5B] = 0x3C,  // [
    [0x7E] = 0x3D,  // ~
    [0x5D] = 0x3E,  // ]
    [0x7C] = 0x40,  // |
};

static void init_gsm7_reverse(void)
{
    static bool inited = false;
    if (inited) return;
    memset(s_gsm7_reverse, 255, sizeof(s_gsm7_reverse));
    for (int i = 0; i < 128; i++) {
        unsigned char c = (unsigned char)gsm7_basic[i];
        s_gsm7_reverse[c] = (uint8_t)i;
    }
    inited = true;
}

// 判断是否可全部用 GSM 7-bit 编码（单字节 + 基本集或扩展集）
static bool can_encode_gsm7(const char *text, int *out_septet_count)
{
    init_gsm7_reverse();
    int septets = 0;
    for (const uint8_t *p = (const uint8_t *)text; *p; p++) {
        if (*p >= 0x80) return false;  // 多字节 UTF-8 需用 UCS-2
        uint8_t s = s_gsm7_reverse[*p];
        if (s != 255) {
            septets += 1;
        } else if (gsm7_extended[*p] != 0) {
            septets += 2;  // ESC + 扩展字符
        } else {
            return false;
        }
    }
    if (out_septet_count) *out_septet_count = septets;
    return true;
}

// 编码 GSM 7-bit：输出为已打包的字节流，返回字节数
static int encode_gsm7(const char *text, uint8_t *out, int out_size)
{
    init_gsm7_reverse();
    int bit_pos = 0;
    memset(out, 0, out_size);
    for (const char *p = text; *p; p++) {
        uint8_t c = (uint8_t)*p;
        uint8_t s1 = s_gsm7_reverse[c];
        int n_septets = 1;
        uint8_t septets[2];
        if (s1 != 255) {
            septets[0] = s1;
        } else if (gsm7_extended[c] != 0) {
            septets[0] = 0x1B;
            septets[1] = gsm7_extended[c];
            n_septets = 2;
        } else {
            septets[0] = 0x3F;  // 回退为 ?
        }
        for (int i = 0; i < n_septets; i++) {
            uint8_t val = septets[i];
            int byte_idx = bit_pos / 8;
            int bit_offset = bit_pos % 8;
            if (byte_idx >= out_size) break;
            out[byte_idx] |= (val << bit_offset) & 0xFF;
            if (bit_offset > 1 && byte_idx + 1 < out_size)
                out[byte_idx + 1] = (val >> (8 - bit_offset)) & 0xFF;
            bit_pos += 7;
        }
    }
    return (bit_pos + 7) / 8;
}

// 编码电话号码为 semi-octet（低半字节先）
static void encode_phone_semi_octet(const char *digits, char *hex_out)
{
    int len = (int)strlen(digits);
    int pos = 0;
    for (int i = 0; i < len; i += 2) {
        if (i + 1 < len) {
            hex_out[pos++] = digits[i + 1];
            hex_out[pos++] = digits[i];
        } else {
            hex_out[pos++] = 'F';
            hex_out[pos++] = digits[i];
        }
    }
    hex_out[pos] = '\0';
}

// 统计 UCS-2 符号数（UTF-8 解码后字符数，用于 70 字限制）
static int utf8_ucs2_symbol_count(const char *utf8)
{
    int n = 0;
    const uint8_t *p = (const uint8_t *)utf8;
    while (*p) {
        if (*p < 0x80) { p++; n++; }
        else if ((*p & 0xE0) == 0xC0) { p += 2; n++; }
        else if ((*p & 0xF0) == 0xE0) { p += 3; n++; }
        else if ((*p & 0xF8) == 0xF0) { p += 4; n++; }  // 含 emoji 等
        else { p++; n++; }
    }
    return n;
}

bool pdu_encode(const char *phone, const char *message, pdu_encode_result_t *result)
{
    if (!phone || !message || !result) {
        s_last_error = PDU_ERR_ADDRESS_FORMAT;
        return false;
    }
    memset(result, 0, sizeof(*result));
    s_last_error = PDU_ERR_OK;

    char norm_phone[20];
    if (!normalize_phone(phone, norm_phone, sizeof(norm_phone))) {
        s_last_error = PDU_ERR_ADDRESS_FORMAT;
        ESP_LOGE(TAG, "号码格式错误");
        return false;
    }

    const char *num = norm_phone[0] == '+' ? norm_phone + 1 : norm_phone;
    bool international = (norm_phone[0] == '+');
    int digits = (int)strlen(num);

    char *hex = result->hex;
    int pos = 0;

    // SCA: 00 = 使用默认短信中心（与 PDUlib setSCAnumber() 一致）
    hex[pos++] = '0'; hex[pos++] = '0';

    // 首字节 PDU 类型: SMS-SUBMIT, 无更多消息, 无 VP 域 (0x01)
    hex[pos++] = '0'; hex[pos++] = '1';

    // MR
    hex[pos++] = '0'; hex[pos++] = '0';

    // DA 长度（数字个数）
    char dh[4];
    byte_to_hex((uint8_t)digits, dh);
    hex[pos++] = dh[0]; hex[pos++] = dh[1];

    // 地址类型：91=国际 81=国内/未知
    hex[pos++] = international ? '9' : '8';
    hex[pos++] = '1';

    char phone_hex[32];
    encode_phone_semi_octet(num, phone_hex);
    for (int i = 0; phone_hex[i]; i++) hex[pos++] = phone_hex[i];

    // PID
    hex[pos++] = '0'; hex[pos++] = '0';

    int septet_count = 0;
    bool use_gsm7 = can_encode_gsm7(message, &septet_count);

    if (use_gsm7) {
        if (septet_count > 160) {
            s_last_error = PDU_ERR_GSM7_TOO_LONG;
            ESP_LOGE(TAG, "7-bit 消息超过 160 字");
            return false;
        }
        // DCS: 00 = GSM 7-bit
        hex[pos++] = '0'; hex[pos++] = '0';
        byte_to_hex((uint8_t)septet_count, dh);
        hex[pos++] = dh[0]; hex[pos++] = dh[1];
        uint8_t gsm_buf[256];
        int gsm_bytes = encode_gsm7(message, gsm_buf, sizeof(gsm_buf));
        if (gsm_bytes > 160) {
            s_last_error = PDU_ERR_BUFFER_TOO_SMALL;
            return false;
        }
        for (int i = 0; i < gsm_bytes; i++) {
            byte_to_hex(gsm_buf[i], dh);
            hex[pos++] = dh[0]; hex[pos++] = dh[1];
        }
    } else {
        int syms = utf8_ucs2_symbol_count(message);
        if (syms > 70) {
            s_last_error = PDU_ERR_UCS2_TOO_LONG;
            ESP_LOGE(TAG, "UCS-2 消息超过 70 字");
            return false;
        }
        hex[pos++] = '0'; hex[pos++] = '8';  // DCS UCS-2
        uint8_t ucs2_buf[320];
        int ucs2_len = utf8_to_ucs2(message, ucs2_buf, sizeof(ucs2_buf));
        if (ucs2_len > 140) {
            s_last_error = PDU_ERR_BUFFER_TOO_SMALL;
            return false;
        }
        byte_to_hex((uint8_t)ucs2_len, dh);
        hex[pos++] = dh[0]; hex[pos++] = dh[1];
        for (int i = 0; i < ucs2_len; i++) {
            byte_to_hex(ucs2_buf[i], dh);
            hex[pos++] = dh[0]; hex[pos++] = dh[1];
        }
    }

    hex[pos] = '\0';

    // TPDU 长度 = 从首字节到末尾的字节数（3GPP 23.040 中 CMGS 参数）
    result->tpdu_len = (pos - 2) / 2;

    ESP_LOGI(TAG, "PDU 编码完成: tpdu_len=%d, %s", result->tpdu_len, use_gsm7 ? "7-bit" : "UCS-2");
    return true;
}