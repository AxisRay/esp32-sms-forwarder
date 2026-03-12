#pragma once

#include "config.h"

// 使用指定通道的 SMTP 参数发送邮件（推送通道 PUSH_TYPE_SMTP）
void smtp_send_email_with_params(const push_params_smtp_t *params, const char *subject, const char *body);
