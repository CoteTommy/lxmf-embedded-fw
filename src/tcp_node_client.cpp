#include "tcp_node_client.h"

#include <WiFi.h>

#include "lxmf_log.h"
#include "native_runtime_bridge.h"

namespace {

constexpr uint32_t kBackoffMs[] = {1000, 2000, 5000, 10000, 30000};
constexpr size_t kMaxInboundFrameBytes = 1024;
constexpr uint8_t kFrameKindCaptureCommand = 0x41;
constexpr uint8_t kFrameKindCaptureResult = 0x42;
constexpr uint8_t kFrameKindCaptureAttachmentChunk = 0x43;
constexpr uint8_t kFrameKindCaptureAttachmentDone = 0x44;
constexpr size_t kPacketHeaderLen = 14;
constexpr size_t kCaptureChunkPayloadBytes = 512;

Stream* g_log_stream = nullptr;
const NodeRuntimeConfig* g_config = nullptr;
WiFiClient g_client;
TcpNodeClientStats g_stats;
uint8_t g_recv_buf[kMaxInboundFrameBytes];
size_t g_recv_len = 0;
size_t g_expected_len = 0;
uint32_t g_next_attempt_ms = 0;
size_t g_backoff_index = 0;
uint32_t g_next_wifi_retry_ms = 0;
wl_status_t g_last_wifi_status = WL_IDLE_STATUS;
bool g_capture_requested = false;
uint32_t g_outbound_sequence = 1;

const char* wifi_status_name(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "idle";
    case WL_NO_SSID_AVAIL:
      return "no_ssid";
    case WL_SCAN_COMPLETED:
      return "scan_completed";
    case WL_CONNECTED:
      return "connected";
    case WL_CONNECT_FAILED:
      return "connect_failed";
    case WL_CONNECTION_LOST:
      return "connection_lost";
    case WL_DISCONNECTED:
      return "disconnected";
    default:
      return "unknown";
  }
}

void log_line(const char* fmt, ...) {
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  const char* message = buf;
  static const char* kPrefix = "[lxmf-net] ";
  const size_t prefix_len = 11;
  if (strncmp(buf, kPrefix, prefix_len) == 0) {
    message = buf + prefix_len;
  }
  lxmf_log_eventf("net", "log", "%s", message);
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
    g_stats.wifi_status = static_cast<uint8_t>(WL_DISCONNECTED);
    return;
  }
  wl_status_t status = WiFi.status();
  wl_status_t previous_status = g_last_wifi_status;
  g_stats.wifi_status = static_cast<uint8_t>(status);
  if (status != g_last_wifi_status) {
    log_line("[lxmf-net] wifi status=%s code=%d", wifi_status_name(status), static_cast<int>(status));
    g_last_wifi_status = status;
  }
  if (status == WL_CONNECTED) {
    g_stats.wifi_connected = true;
    if (previous_status != WL_CONNECTED) {
      log_line("[lxmf-net] wifi connected ip=%s rssi=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    return;
  }
  g_stats.wifi_connected = false;
  uint32_t now_ms = millis();
  if (now_ms < g_next_wifi_retry_ms) {
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  g_stats.wifi_connect_attempts++;
  log_line("[lxmf-net] wifi begin ssid=%s attempt=%lu", g_config->wifi_ssid, (unsigned long)g_stats.wifi_connect_attempts);
  WiFi.begin(g_config->wifi_ssid, g_config->wifi_password);
  g_next_wifi_retry_ms = now_ms + 10000;
}

void connect_tcp(uint32_t now_ms) {
  if (!mode_enabled() || !config_ready() || !g_stats.wifi_connected || g_client.connected()) {
    return;
  }
  if (now_ms < g_next_attempt_ms) {
    return;
  }

  g_stats.tcp_connect_attempts++;
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

bool write_packet_frame(uint8_t kind, const uint8_t* payload, size_t payload_len) {
  if (!g_client.connected() || payload == nullptr || payload_len == 0) {
    return false;
  }
  if (payload_len > 0xFFFF || (kPacketHeaderLen + payload_len) > 0xFFFF) {
    log_line("[lxmf-net] outbound frame too large kind=0x%02x payload_bytes=%u",
             (unsigned)kind,
             (unsigned)payload_len);
    return false;
  }

  const uint32_t sequence = g_outbound_sequence++;
  const uint16_t encoded_len = static_cast<uint16_t>(kPacketHeaderLen + payload_len);
  uint8_t len_prefix[2] = {
      static_cast<uint8_t>((encoded_len >> 8) & 0xFF),
      static_cast<uint8_t>(encoded_len & 0xFF),
  };
  uint8_t header[kPacketHeaderLen] = {
      'R', 'N', 'E', '1', 0x01, kind,
      static_cast<uint8_t>(sequence & 0xFF),
      static_cast<uint8_t>((sequence >> 8) & 0xFF),
      static_cast<uint8_t>((sequence >> 16) & 0xFF),
      static_cast<uint8_t>((sequence >> 24) & 0xFF),
      static_cast<uint8_t>(payload_len & 0xFF),
      static_cast<uint8_t>((payload_len >> 8) & 0xFF),
      static_cast<uint8_t>((payload_len >> 16) & 0xFF),
      static_cast<uint8_t>((payload_len >> 24) & 0xFF),
  };

  if (g_client.write(len_prefix, sizeof(len_prefix)) != sizeof(len_prefix)
      || g_client.write(header, sizeof(header)) != sizeof(header)
      || g_client.write(payload, payload_len) != payload_len) {
    log_line("[lxmf-net] tcp write failed kind=0x%02x", (unsigned)kind);
    mark_transport_down();
    return false;
  }
  g_stats.tx_frames++;
  return true;
}

bool handle_inbound_packet_frame(const uint8_t* bytes, size_t len) {
  if (len < kPacketHeaderLen) {
    return false;
  }
  if (!(bytes[0] == 'R' && bytes[1] == 'N' && bytes[2] == 'E' && bytes[3] == '1')) {
    log_line("[lxmf-net] invalid packet magic");
    return false;
  }
  if (bytes[4] != 0x01) {
    log_line("[lxmf-net] unsupported packet version=%u", (unsigned)bytes[4]);
    return false;
  }
  const uint8_t kind = bytes[5];
  const uint32_t payload_len = static_cast<uint32_t>(bytes[10])
      | (static_cast<uint32_t>(bytes[11]) << 8)
      | (static_cast<uint32_t>(bytes[12]) << 16)
      | (static_cast<uint32_t>(bytes[13]) << 24);
  if (payload_len == 0 || len != (kPacketHeaderLen + payload_len)) {
    log_line("[lxmf-net] invalid packet payload len=%u", (unsigned)payload_len);
    return false;
  }
  if (kind == kFrameKindCaptureCommand) {
    g_capture_requested = true;
    g_stats.rx_frames++;
    log_line("[lxmf-net] capture command received payload_bytes=%u", (unsigned)payload_len);
    return true;
  }
  if (native_runtime_bridge_push_inbound_wire(bytes, len)) {
    g_stats.rx_frames++;
    return true;
  }
  log_line("[lxmf-net] inbound runtime frame rejected bytes=%u", (unsigned)len);
  return false;
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
    handle_inbound_packet_frame(g_recv_buf, g_expected_len);
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
  g_next_wifi_retry_ms = 0;
  g_last_wifi_status = WL_IDLE_STATUS;
  g_capture_requested = false;
  g_outbound_sequence = 1;
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

bool tcp_node_client_connected() {
  return g_stats.tcp_connected && g_client.connected();
}

bool tcp_node_client_take_capture_request() {
  bool requested = g_capture_requested;
  g_capture_requested = false;
  return requested;
}

bool tcp_node_client_send_capture(const uint8_t* jpeg, size_t len, uint16_t width, uint16_t height) {
  if (!tcp_node_client_connected() || jpeg == nullptr || len == 0) {
    return false;
  }

  const uint16_t total_chunks =
      static_cast<uint16_t>((len + kCaptureChunkPayloadBytes - 1) / kCaptureChunkPayloadBytes);
  const uint16_t chunk_bytes = static_cast<uint16_t>(kCaptureChunkPayloadBytes);

  uint8_t result_payload[11] = {
      0x00,
      static_cast<uint8_t>(len & 0xFF),
      static_cast<uint8_t>((len >> 8) & 0xFF),
      static_cast<uint8_t>((len >> 16) & 0xFF),
      static_cast<uint8_t>((len >> 24) & 0xFF),
      static_cast<uint8_t>(chunk_bytes & 0xFF),
      static_cast<uint8_t>((chunk_bytes >> 8) & 0xFF),
      static_cast<uint8_t>(width & 0xFF),
      static_cast<uint8_t>((width >> 8) & 0xFF),
      static_cast<uint8_t>(height & 0xFF),
      static_cast<uint8_t>((height >> 8) & 0xFF),
  };
  if (!write_packet_frame(kFrameKindCaptureResult, result_payload, sizeof(result_payload))) {
    return false;
  }

  size_t offset = 0;
  uint16_t seq = 0;
  while (offset < len) {
    const uint16_t payload_bytes =
        static_cast<uint16_t>(min(static_cast<size_t>(kCaptureChunkPayloadBytes), len - offset));
    uint8_t chunk_payload[6 + kCaptureChunkPayloadBytes];
    chunk_payload[0] = static_cast<uint8_t>(seq & 0xFF);
    chunk_payload[1] = static_cast<uint8_t>((seq >> 8) & 0xFF);
    chunk_payload[2] = static_cast<uint8_t>(total_chunks & 0xFF);
    chunk_payload[3] = static_cast<uint8_t>((total_chunks >> 8) & 0xFF);
    chunk_payload[4] = static_cast<uint8_t>(payload_bytes & 0xFF);
    chunk_payload[5] = static_cast<uint8_t>((payload_bytes >> 8) & 0xFF);
    memcpy(chunk_payload + 6, jpeg + offset, payload_bytes);
    if (!write_packet_frame(
            kFrameKindCaptureAttachmentChunk, chunk_payload, static_cast<size_t>(6 + payload_bytes))) {
      return false;
    }
    offset += payload_bytes;
    seq++;
  }

  uint8_t done_payload[6] = {
      static_cast<uint8_t>(total_chunks & 0xFF),
      static_cast<uint8_t>((total_chunks >> 8) & 0xFF),
      static_cast<uint8_t>(len & 0xFF),
      static_cast<uint8_t>((len >> 8) & 0xFF),
      static_cast<uint8_t>((len >> 16) & 0xFF),
      static_cast<uint8_t>((len >> 24) & 0xFF),
  };
  return write_packet_frame(kFrameKindCaptureAttachmentDone, done_payload, sizeof(done_payload));
}
