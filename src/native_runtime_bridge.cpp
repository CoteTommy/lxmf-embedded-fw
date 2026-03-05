#include "native_runtime_bridge.h"

#include <stdarg.h>
#include <stdio.h>

#if __has_include("rns_embedded_ffi.h")
#define LXMF_HAS_RUST_FFI 1
#include "rns_embedded_ffi.h"
#else
#define LXMF_HAS_RUST_FFI 0
#endif

namespace {

Stream* g_log_stream = nullptr;
NativeRuntimeBridgeStats g_stats;
bool g_connected = false;
uint32_t g_last_announce_ms = 0;

#if LXMF_HAS_RUST_FFI
RnsEmbeddedNode* g_node = nullptr;

RnsEmbeddedStatus ensure_node() {
  if (g_node != nullptr) {
    return RNS_EMBEDDED_STATUS_OK;
  }
  RnsEmbeddedNodeConfig config = rns_embedded_node_config_default();
  g_node = rns_embedded_node_new(&config);
  return g_node == nullptr ? RNS_EMBEDDED_STATUS_INVALID_STATE : RNS_EMBEDDED_STATUS_OK;
}

void log_line(const char* fmt, ...) {
  if (g_log_stream == nullptr) {
    return;
  }
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  g_log_stream->println(buf);
}
#else
void log_line(const char* fmt, ...) {
  if (g_log_stream == nullptr) {
    return;
  }
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  g_log_stream->println(buf);
}
#endif

}  // namespace

void native_runtime_bridge_init(Stream* log_stream) {
  g_log_stream = log_stream;
  g_stats = NativeRuntimeBridgeStats{};
  g_connected = false;
  g_last_announce_ms = 0;
#if LXMF_HAS_RUST_FFI
  if (g_node != nullptr) {
    rns_embedded_node_free(g_node);
    g_node = nullptr;
  }
  RnsEmbeddedStatus status = ensure_node();
  log_line("[lxmf-native] init backend=%s status=%d", native_runtime_bridge_backend_name(), (int)status);
#else
  log_line("[lxmf-native] init backend=%s", native_runtime_bridge_backend_name());
#endif
}

void native_runtime_bridge_set_link_state(bool connected) {
  g_connected = connected;
#if LXMF_HAS_RUST_FFI
  if (ensure_node() != RNS_EMBEDDED_STATUS_OK) {
    return;
  }
  rns_embedded_node_set_link_state(
      g_node,
      connected ? RNS_EMBEDDED_LINK_UP : RNS_EMBEDDED_LINK_DOWN);
#endif
}

void native_runtime_bridge_tick(uint32_t now_ms) {
  g_stats.ticks++;
#if LXMF_HAS_RUST_FFI
  if (ensure_node() != RNS_EMBEDDED_STATUS_OK) {
    return;
  }
  RnsEmbeddedStatus status = rns_embedded_node_tick(g_node, now_ms);
  if (status != RNS_EMBEDDED_STATUS_OK && g_log_stream != nullptr) {
    log_line("[lxmf-native] tick status=%d", (int)status);
  }
#else
  if (g_connected && (now_ms - g_last_announce_ms) >= 30000) {
    g_last_announce_ms = now_ms;
    g_stats.outbound_frames++;
    g_stats.last_sequence++;
    log_line("[lxmf-native] stub announce sequence=%lu", (unsigned long)g_stats.last_sequence);
  }
#endif
}

bool native_runtime_bridge_push_inbound_wire(const uint8_t* data, size_t len) {
  if (data == nullptr || len == 0) {
    return false;
  }
  g_stats.inbound_frames++;
#if LXMF_HAS_RUST_FFI
  if (ensure_node() != RNS_EMBEDDED_STATUS_OK) {
    return false;
  }
  return rns_embedded_node_push_inbound_wire(g_node, data, len) == RNS_EMBEDDED_STATUS_OK;
#else
  log_line("[lxmf-native] stub inbound bytes=%u", (unsigned)len);
  return true;
#endif
}

bool native_runtime_bridge_queue_message(const uint8_t destination[16], const uint8_t* body, size_t len) {
  if (destination == nullptr || (body == nullptr && len != 0)) {
    return false;
  }
  g_stats.queue_message_calls++;
#if LXMF_HAS_RUST_FFI
  if (ensure_node() != RNS_EMBEDDED_STATUS_OK) {
    return false;
  }
  uint32_t sequence = 0;
  RnsEmbeddedStatus status =
      rns_embedded_node_queue_message(g_node, destination, body, len, &sequence);
  if (status == RNS_EMBEDDED_STATUS_OK) {
    g_stats.last_sequence = sequence;
    return true;
  }
  if (g_log_stream != nullptr) {
    log_line("[lxmf-native] queue_message status=%d", (int)status);
  }
  return false;
#else
  g_stats.last_sequence++;
  log_line(
      "[lxmf-native] stub queue_message bytes=%u sequence=%lu",
      (unsigned)len,
      (unsigned long)g_stats.last_sequence);
  return true;
#endif
}

bool native_runtime_bridge_take_outbound_wire(uint8_t* out, size_t capacity, size_t* out_len) {
  if (out_len == nullptr) {
    return false;
  }
  *out_len = 0;
#if LXMF_HAS_RUST_FFI
  if (ensure_node() != RNS_EMBEDDED_STATUS_OK) {
    return false;
  }
  RnsEmbeddedStatus status = rns_embedded_node_take_outbound_wire(g_node, out, capacity, out_len);
  if (status == RNS_EMBEDDED_STATUS_OK) {
    g_stats.outbound_frames++;
    return true;
  }
  return status != RNS_EMBEDDED_STATUS_NOT_FOUND ? false : false;
#else
  (void)out;
  (void)capacity;
  return false;
#endif
}

NativeRuntimeBridgeStats native_runtime_bridge_stats() {
  return g_stats;
}

const char* native_runtime_bridge_backend_name() {
#if LXMF_HAS_RUST_FFI
  return "rust-ffi";
#else
  return "stub";
#endif
}
