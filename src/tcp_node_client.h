#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "node_runtime_config.h"

struct TcpNodeClientStats {
  bool wifi_connected = false;
  bool tcp_connected = false;
  uint8_t wifi_status = 0;
  uint32_t wifi_connect_attempts = 0;
  uint32_t reconnects = 0;
  uint32_t tcp_connect_attempts = 0;
  uint32_t tx_frames = 0;
  uint32_t rx_frames = 0;
};

void tcp_node_client_init(Stream* log_stream, const NodeRuntimeConfig* config);
void tcp_node_client_tick(uint32_t now_ms);
TcpNodeClientStats tcp_node_client_stats();
bool tcp_node_client_connected();
bool tcp_node_client_take_capture_request();
bool tcp_node_client_send_capture_result(uint8_t status,
                                         uint32_t total_bytes,
                                         uint16_t chunk_bytes,
                                         uint16_t width,
                                         uint16_t height);
bool tcp_node_client_send_capture(const uint8_t* jpeg,
                                  size_t len,
                                  uint16_t width,
                                  uint16_t height);
