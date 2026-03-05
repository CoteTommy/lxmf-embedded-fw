#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "node_runtime_config.h"

struct TcpNodeClientStats {
  bool wifi_connected = false;
  bool tcp_connected = false;
  uint32_t reconnects = 0;
  uint32_t tx_frames = 0;
  uint32_t rx_frames = 0;
};

void tcp_node_client_init(Stream* log_stream, const NodeRuntimeConfig* config);
void tcp_node_client_tick(uint32_t now_ms);
TcpNodeClientStats tcp_node_client_stats();
