// main/voice_net.h —— 语音链路的传输层:Wi-Fi + TCP + 极简帧协议。
//
// 为什么是明文 TCP 而不是 TLS/WebSocket:
//   - 设备只有 ~200 KB 可用 RAM 且无 PSRAM,mbedTLS 握手就要 30~40 KB;
//   - 服务器就在同一个局域网(用户的 PC),由 PC 去连云端并承担 TLS;
//   - 语音要的是低延迟,明文 + TCP_NODELAY 最省事。
//
// 协议(控制用行、音频用长度前缀帧):
//   设备 → PC   HELLO 1 <rate> <bits> <ch>\n
//   PC  → 设备  OK\n
//   设备 → PC   REC\n  然后 [u16 长度][PCM] ... [u16 0]
//   PC  → 设备  PLAY <总字节>\n  然后 [u16 长度][PCM] ... [u16 0]
//   PC  → 设备  TXT <一行文本>\n    (识别结果/回复;UTF-8,换行由 TXT 终止)
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// voice_net_recv_frame() 的专用错误码:帧长非法(通常是服务器在音频帧中间
// 插入了文本行,比如 "TX" 被读成 0x5458 = 21592 字节)。
// 与一般的连接错误分开,好让上层决定是恢复还是重连。
#define VN_FRAME_PROTOCOL_ERROR (-100)

// 连 Wi-Fi → 连服务器 → 完成 HELLO 握手。阻塞,最多约 25 秒。幂等。
esp_err_t voice_net_connect(void);

// 丢弃当前连接并重新走一遍 connect。
esp_err_t voice_net_reconnect(void);

// 只丢弃连接不重连(用于 Wi-Fi 掉线时避免带着死 socket 重试)。
void voice_net_drop(void);

// Wi-Fi 是否已连上并取得 IP(与 socket 状态无关)。
bool voice_net_wifi_up(void);

// Wi-Fi 已连 且 socket 可用。
bool voice_net_ready(void);

// 发一行控制命令(自动补 '\n')。
esp_err_t voice_net_send_line(const char *line);

// 发一帧音频:[u16 大端长度][负载]。
esp_err_t voice_net_send_frame(const void *data, size_t len);

// 发结束标记(长度 0 的帧)。
esp_err_t voice_net_send_end(void);

// 读一帧音频。返回:
//   1  = 读到数据(len_out 为长度)
//   0  = 结束标记
//   VN_FRAME_PROTOCOL_ERROR = 帧长非法(服务器在音频中间插了文本行)
//   <0 (其他) = 连接错误
int voice_net_recv_frame(void *buf, size_t max, size_t *len_out);

// 读一行(去掉 '\n',补 '\0')。返回长度,<0 错误。
int voice_net_recv_line(char *buf, size_t max);
