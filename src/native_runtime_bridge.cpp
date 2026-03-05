#include "native_runtime_bridge.h"
#include "lxmf_log.h"

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
NativeNodeMode g_node_mode = NATIVE_NODE_MODE_BLE_ONLY;
const char* g_lifecycle_name = "boot";

#if LXMF_HAS_RUST_FFI
RnsEmbeddedNode* g_node = nullptr;

RnsEmbeddedStatus ensure_node() {
  if (g_node != nullptr) {
    return RNS_EMBEDDED_STATUS_OK;
  }
  RnsEmbeddedNodeConfig config = rns_embedded_node_config_default();
  switch (g_node_mode) {
    case NATIVE_NODE_MODE_BLE_ONLY:
      config.node_mode = RNS_EMBEDDED_NODE_MODE_BLE_ONLY;
      break;
    case NATIVE_NODE_MODE_TCP_CLIENT:
      config.node_mode = RNS_EMBEDDED_NODE_MODE_TCP_CLIENT;
      break;
    case NATIVE_NODE_MODE_TCP_SERVER:
      config.node_mode = RNS_EMBEDDED_NODE_MODE_TCP_SERVER;
      break;
    default:
      config.node_mode = RNS_EMBEDDED_NODE_MODE_BLE_ONLY;
      break;
  }
  g_node = rns_embedded_node_new(&config);
  return g_node == nullptr ? RNS_EMBEDDED_STATUS_INVALID_STATE : RNS_EMBEDDED_STATUS_OK;
}

void log_line(const char* event, const char* fmt, ...) {
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  lxmf_log_eventf("native", event, "%s", buf);
}
#else
void log_line(const char* event, const char* fmt, ...) {
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  lxmf_log_eventf("native", event, "%s", buf);
}
#endif

}  // namespace

void native_runtime_bridge_init(Stream* log_stream) {
  g_log_stream = log_stream;
  g_stats = NativeRuntimeBridgeStats{};
  g_connected = false;
  g_last_announce_ms = 0;
  g_node_mode = NATIVE_NODE_MODE_BLE_ONLY;
  g_lifecycle_name = "boot";
#if LXMF_HAS_RUST_FFI
  if (g_node != nullptr) {
    rns_embedded_node_free(g_node);
    g_node = nullptr;
  }
  RnsEmbeddedStatus status = ensure_node();
  log_line("init", "init backend=%s status=%d", native_runtime_bridge_backend_name(), (int)status);
#else
  log_line("init", "init backend=%s", native_runtime_bridge_backend_name());
#endif
}

void native_runtime_bridge_set_node_mode(NativeNodeMode mode) {
  g_node_mode = mode;
#if LXMF_HAS_RUST_FFI
  if (g_node != nullptr) {
    rns_embedded_node_free(g_node);
    g_node = nullptr;
  }
  ensure_node();
#endif
}

void native_runtime_bridge_set_network_provisioned(bool provisioned) {
  g_stats.network_provisioned = provisioned;
#if LXMF_HAS_RUST_FFI
  if (ensure_node() != RNS_EMBEDDED_STATUS_OK) {
    return;
  }
  rns_embedded_node_set_network_provisioned(g_node, provisioned);
#endif
}

void native_runtime_bridge_set_ble_recovery_active(bool active) {
  g_stats.ble_recovery_active = active;
#if LXMF_HAS_RUST_FFI
  if (ensure_node() != RNS_EMBEDDED_STATUS_OK) {
    return;
  }
  rns_embedded_node_set_ble_recovery_active(g_node, active);
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
  RnsEmbeddedLifecycleState lifecycle = rns_embedded_node_get_lifecycle_state(g_node);
  switch (lifecycle) {
    case RNS_EMBEDDED_LIFECYCLE_BOOT:
      g_lifecycle_name = "boot";
      break;
    case RNS_EMBEDDED_LIFECYCLE_UNPROVISIONED:
      g_lifecycle_name = "unprovisioned";
      break;
    case RNS_EMBEDDED_LIFECYCLE_PROVISIONED_OFFLINE:
      g_lifecycle_name = "provisioned_offline";
      break;
    case RNS_EMBEDDED_LIFECYCLE_TCP_ONLINE:
      g_lifecycle_name = "tcp_online";
      break;
    case RNS_EMBEDDED_LIFECYCLE_BLE_RECOVERY:
      g_lifecycle_name = "ble_recovery";
      break;
    case RNS_EMBEDDED_LIFECYCLE_FAILURE_RECONNECT:
      g_lifecycle_name = "failure_reconnect";
      break;
    default:
      g_lifecycle_name = "unknown";
      break;
  }
  if (status != RNS_EMBEDDED_STATUS_OK && g_log_stream != nullptr) {
    log_line("tick_status", "tick status=%d", (int)status);
  }
#else
  if (g_connected && (now_ms - g_last_announce_ms) >= 30000) {
    g_last_announce_ms = now_ms;
    g_stats.outbound_frames++;
    g_stats.last_sequence++;
    log_line("stub_announce", "stub announce sequence=%lu", (unsigned long)g_stats.last_sequence);
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
  log_line("stub_inbound", "stub inbound bytes=%u", (unsigned)len);
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
    log_line("queue_message_status", "queue_message status=%d", (int)status);
  }
  return false;
#else
  g_stats.last_sequence++;
  log_line(
      "stub_queue_message",
      "stub queue_message bytes=%u sequence=%lu",
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

const char* native_runtime_bridge_mode_name() {
  switch (g_node_mode) {
    case NATIVE_NODE_MODE_BLE_ONLY:
      return "ble_only";
    case NATIVE_NODE_MODE_TCP_CLIENT:
      return "tcp_client";
    case NATIVE_NODE_MODE_TCP_SERVER:
      return "tcp_server";
    default:
      return "unknown";
  }
}

const char* native_runtime_bridge_lifecycle_name() {
  return g_lifecycle_name;
}
