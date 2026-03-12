#pragma once

#include "config.h"

// 发送到指定推送通道
void push_send_to_channel(const push_channel_t *channel,
                           const char *sender, const char *message,
                           const char *timestamp);

// 发送到所有启用的推送通道
void push_send_to_all(const char *sender, const char *message,
                       const char *timestamp);
