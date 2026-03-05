#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

struct NativeRuntimeBridgeStats {
  uint32_t ticks = 0;
  uint32_t outbound_frames = 0;
  uint32_t inbound_frames = 0;
  uint32_t queue_message_calls = 0;
  uint32_t last_sequence = 0;
};

void native_runtime_bridge_init(Stream* log_stream);
void native_runtime_bridge_set_link_state(bool connected);
void native_runtime_bridge_tick(uint32_t now_ms);
bool native_runtime_bridge_push_inbound_wire(const uint8_t* data, size_t len);
bool native_runtime_bridge_queue_message(const uint8_t destination[16], const uint8_t* body, size_t len);
bool native_runtime_bridge_take_outbound_wire(uint8_t* out, size_t capacity, size_t* out_len);
NativeRuntimeBridgeStats native_runtime_bridge_stats();
const char* native_runtime_bridge_backend_name();
