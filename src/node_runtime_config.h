#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "native_runtime_bridge.h"

struct NodeRuntimeConfig {
  NativeNodeMode node_mode = NATIVE_NODE_MODE_BLE_ONLY;
  bool ble_recovery_enabled = true;
  char wifi_ssid[33] = {0};
  char wifi_password[65] = {0};
  char tcp_host[65] = {0};
  uint16_t tcp_port = 7443;
};

bool node_runtime_config_load(NodeRuntimeConfig* config);
bool node_runtime_config_save(const NodeRuntimeConfig& config);
bool node_runtime_config_has_wifi(const NodeRuntimeConfig& config);
bool node_runtime_config_has_tcp_client_target(const NodeRuntimeConfig& config);
const char* node_runtime_config_mode_name(const NodeRuntimeConfig& config);
