#include "tcp_node_client.h"

#include <WiFi.h>

#include "native_runtime_bridge.h"

namespace {

constexpr uint32_t kBackoffMs[] = {1000, 2000, 5000, 10000, 30000};
constexpr size_t kMaxInboundFrameBytes = 1024;

Stream* g_log_stream = nullptr;
const NodeRuntimeConfig* g_config = nullptr;
WiFiClient g_client;
TcpNodeClientStats g_stats;
uint8_t g_recv_buf[kMaxInboundFrameBytes];
size_t g_recv_len = 0;
size_t g_expected_len = 0;
uint32_t g_next_attempt_ms = 0;
size_t g_backoff_index = 0;

void log_line(const char* fmt, ...) {
  if (g_log_stream == nullptr) {
    return;
  }
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  g_log_stream->println(buf);
}

bool mode_enabled() {
  return g_config != nullptr && g_config->node_mode == NATIVE_NODE_MODE_TCP_CLIENT;
}

bool config_ready() {
  return g_config != nullptr
      && node_runtime_config_has_wifi(*g_config)
      && node_runtime_config_has_tcp_client_target(*g_config);
}

void mark_transport_down() {
  if (g_client.connected()) {
    g_client.stop();
  }
  g_stats.tcp_connected = false;
  g_recv_len = 0;
  g_expected_len = 0;
  native_runtime_bridge_set_link_state(false);
}

void ensure_wifi() {
  if (!mode_enabled() || !config_ready()) {
    g_stats.wifi_connected = false;
    return;
  }
  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    g_stats.wifi_connected = true;
    return;
  }
  g_stats.wifi_connected = false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(g_config->wifi_ssid, g_config->wifi_password);
}

void connect_tcp(uint32_t now_ms) {
  if (!mode_enabled() || !config_ready() || !g_stats.wifi_connected || g_client.connected()) {
    return;
  }
  if (now_ms < g_next_attempt_ms) {
    return;
  }

  log_line("[lxmf-net] tcp connect host=%s port=%u", g_config->tcp_host, g_config->tcp_port);
  if (g_client.connect(g_config->tcp_host, g_config->tcp_port)) {
    g_stats.tcp_connected = true;
    g_stats.reconnects++;
    g_backoff_index = 0;
    g_next_attempt_ms = now_ms + kBackoffMs[0];
    native_runtime_bridge_set_link_state(true);
    log_line("[lxmf-net] tcp connected");
    return;
  }

  mark_transport_down();
  g_next_attempt_ms = now_ms + kBackoffMs[g_backoff_index];
  if (g_backoff_index + 1 < (sizeof(kBackoffMs) / sizeof(kBackoffMs[0]))) {
    g_backoff_index++;
  }
  log_line("[lxmf-net] tcp connect failed retry_in_ms=%lu", (unsigned long)(g_next_attempt_ms - now_ms));
}

void drain_outbound() {
  if (!g_client.connected()) {
    return;
  }
  uint8_t payload[kMaxInboundFrameBytes];
  size_t payload_len = 0;
  while (native_runtime_bridge_take_outbound_wire(payload, sizeof(payload), &payload_len)) {
    if (payload_len == 0 || payload_len > 0xFFFF) {
      continue;
    }
    uint8_t header[2] = {
        static_cast<uint8_t>((payload_len >> 8) & 0xFF),
        static_cast<uint8_t>(payload_len & 0xFF),
    };
    if (g_client.write(header, sizeof(header)) != sizeof(header)
        || g_client.write(payload, payload_len) != payload_len) {
      log_line("[lxmf-net] tcp write failed");
      mark_transport_down();
      return;
    }
    g_stats.tx_frames++;
  }
}

void handle_inbound_byte(uint8_t byte) {
  if (g_expected_len == 0) {
    g_recv_buf[g_recv_len++] = byte;
    if (g_recv_len == 2) {
      g_expected_len = (static_cast<size_t>(g_recv_buf[0]) << 8) | g_recv_buf[1];
      g_recv_len = 0;
      if (g_expected_len == 0 || g_expected_len > sizeof(g_recv_buf)) {
        log_line("[lxmf-net] invalid tcp frame length=%u", (unsigned)g_expected_len);
        g_expected_len = 0;
      }
    }
    return;
  }

  g_recv_buf[g_recv_len++] = byte;
  if (g_recv_len == g_expected_len) {
    if (native_runtime_bridge_push_inbound_wire(g_recv_buf, g_expected_len)) {
      g_stats.rx_frames++;
    } else {
      log_line("[lxmf-net] inbound runtime frame rejected bytes=%u", (unsigned)g_expected_len);
    }
    g_recv_len = 0;
    g_expected_len = 0;
  }
}

void read_inbound() {
  while (g_client.connected() && g_client.available() > 0) {
    int value = g_client.read();
    if (value < 0) {
      break;
    }
    handle_inbound_byte(static_cast<uint8_t>(value));
  }
}

}  // namespace

void tcp_node_client_init(Stream* log_stream, const NodeRuntimeConfig* config) {
  g_log_stream = log_stream;
  g_config = config;
  g_stats = TcpNodeClientStats{};
  g_recv_len = 0;
  g_expected_len = 0;
  g_next_attempt_ms = 0;
  g_backoff_index = 0;
}

void tcp_node_client_tick(uint32_t now_ms) {
  if (!mode_enabled()) {
    mark_transport_down();
    return;
  }

  ensure_wifi();
  native_runtime_bridge_set_network_provisioned(config_ready());

  if (!g_stats.wifi_connected) {
    mark_transport_down();
    return;
  }

  if (!g_client.connected()) {
    mark_transport_down();
    connect_tcp(now_ms);
    return;
  }

  g_stats.tcp_connected = true;
  drain_outbound();
  read_inbound();
}

TcpNodeClientStats tcp_node_client_stats() {
  return g_stats;
}
