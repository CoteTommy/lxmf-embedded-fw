#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEAdvertising.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_bt_device.h>
#include <esp_camera.h>

#include "lxmf_log.h"
#include "native_runtime_bridge.h"
#include "node_runtime_config.h"
#include "tcp_node_client.h"

static const char* DEVICE_NAME = "LXMF-CAM-STUB";
static const char* MANUFACTURER_MARKER = "LXMF01";

static const char* SERVICE_UUID = "12345678-1234-1234-1234-1234567890ab";
static const char* WRITE_CHAR_UUID = "12345678-1234-1234-1234-1234567890ac";
static const char* NOTIFY_CHAR_UUID = "12345678-1234-1234-1234-1234567890ad";

static const uint8_t FRAME_CAPTURE_REQ = 0x02;
static const uint8_t FRAME_CAPTURE_ACK = 0x03;
static const uint8_t FRAME_CHUNK = 0x04;
static const uint8_t FRAME_CHUNK_ACK = 0x05;
static const uint8_t FRAME_DONE = 0x06;
static const uint8_t FRAME_ERROR = 0x07;
static const uint8_t FRAME_NACK = 0x08;
static const uint8_t FRAME_NATIVE_ANNOUNCE_REQ = 0x21;
static const uint8_t FRAME_NATIVE_MESSAGE_TX_REQ = 0x22;
static const uint8_t FRAME_NATIVE_WIRE = 0x23;
static const uint16_t BLE_CHUNK_PAYLOAD_BYTES = 5;
static const uint8_t BLE_ACK_EVERY_CHUNKS = 4;
static const uint32_t BLE_ACK_TIMEOUT_MS = 900;
static const uint8_t BLE_NOTIFY_DELAY_MS = 8;
static const size_t BLE_ACK_QUEUE_CAPACITY = 32;
static const size_t BLE_NATIVE_WIRE_MAX_BYTES = 180;

static BLECharacteristic* g_notify = nullptr;
static volatile bool g_capture_requested = false;
static NodeRuntimeConfig g_node_config;

static uint32_t g_transfer_id = 1;
static bool g_connected = false;
static bool g_native_node_enabled = true;
static bool g_ble_transport_enabled = false;

enum FallbackReasonCode : uint8_t {
  FALLBACK_NONE = 0,
  FALLBACK_ACK_TIMEOUT = 1,
  FALLBACK_CAMERA_ERROR = 2,
  FALLBACK_QUEUE_BACKPRESSURE = 3,
};

struct NativeNodeDiagnostics {
  uint32_t announce_sent = 0;
  uint32_t message_tx = 0;
  uint32_t message_rx = 0;
  uint32_t chunk_retry_total = 0;
  uint32_t drop_invalid = 0;
  uint32_t drop_seq_gap = 0;
  uint32_t drop_disconnected = 0;
  uint32_t drop_backpressure = 0;
  FallbackReasonCode fallback_reason = FALLBACK_NONE;
};

static NativeNodeDiagnostics g_diag;

struct AckEvent {
  uint8_t type;
  uint32_t transfer_id;
  uint16_t seq;
};

static volatile AckEvent g_ack_queue[BLE_ACK_QUEUE_CAPACITY];
static volatile uint8_t g_ack_queue_head = 0;
static volatile uint8_t g_ack_queue_tail = 0;
static const uint8_t NATIVE_DESTINATION_STUB[16] = {
  0x4C, 0x58, 0x4D, 0x46, 0x2D, 0x45, 0x53, 0x50,
  0x33, 0x32, 0x2D, 0x4E, 0x4F, 0x44, 0x45, 0x01
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    (void)server;
    g_connected = true;
    native_runtime_bridge_set_ble_recovery_active(true);
    if (g_node_config.node_mode == NATIVE_NODE_MODE_BLE_ONLY) {
      native_runtime_bridge_set_link_state(true);
    }
    lxmf_log_eventf("ble", "client_connected", "ble client connected");
  }

  void onDisconnect(BLEServer* server) override {
    (void)server;
    g_connected = false;
    native_runtime_bridge_set_ble_recovery_active(false);
    if (g_node_config.node_mode == NATIVE_NODE_MODE_BLE_ONLY) {
      native_runtime_bridge_set_link_state(false);
    }
    lxmf_log_eventf("ble", "client_disconnected", "ble client disconnected");
    if (g_ble_transport_enabled) {
      BLEDevice::startAdvertising();
      lxmf_log_eventf("ble", "advertising_restarted", "advertising restarted");
    }
  }
};

static bool enqueue_ack_event(uint8_t type, uint32_t transfer_id, uint16_t seq) {
  bool queued = false;
  noInterrupts();
  uint8_t next_head = (uint8_t)((g_ack_queue_head + 1) % BLE_ACK_QUEUE_CAPACITY);
  if (next_head != g_ack_queue_tail) {
    g_ack_queue[g_ack_queue_head].type = type;
    g_ack_queue[g_ack_queue_head].transfer_id = transfer_id;
    g_ack_queue[g_ack_queue_head].seq = seq;
    g_ack_queue_head = next_head;
    queued = true;
  }
  interrupts();
  if (!queued) {
    g_diag.drop_backpressure++;
  }
  return queued;
}

static bool dequeue_ack_event(AckEvent* event) {
  bool found = false;
  noInterrupts();
  if (g_ack_queue_head != g_ack_queue_tail) {
    event->type = g_ack_queue[g_ack_queue_tail].type;
    event->transfer_id = g_ack_queue[g_ack_queue_tail].transfer_id;
    event->seq = g_ack_queue[g_ack_queue_tail].seq;
    g_ack_queue_tail = (uint8_t)((g_ack_queue_tail + 1) % BLE_ACK_QUEUE_CAPACITY);
    found = true;
  }
  interrupts();
  return found;
}

static void notify_bytes(const uint8_t* data, size_t len) {
  if (g_notify == nullptr) {
    return;
  }
  g_notify->setValue(const_cast<uint8_t*>(data), len);
  g_notify->notify();
  delay(BLE_NOTIFY_DELAY_MS);
}

static void notify_capture_ack() {
  uint8_t frame[1] = {FRAME_CAPTURE_ACK};
  notify_bytes(frame, sizeof(frame));
}

static void notify_done() {
  uint8_t frame[1] = {FRAME_DONE};
  notify_bytes(frame, sizeof(frame));
}

static void notify_error(const char* msg) {
  const size_t max_len = 60;
  size_t msg_len = strnlen(msg, max_len);
  uint8_t frame[1 + max_len];
  frame[0] = FRAME_ERROR;
  memcpy(&frame[1], msg, msg_len);
  notify_bytes(frame, 1 + msg_len);
}

static bool notify_native_wire(const uint8_t* payload, size_t payload_len) {
  if (payload == nullptr || payload_len == 0) {
    return false;
  }
  if (payload_len > BLE_NATIVE_WIRE_MAX_BYTES) {
    Serial.printf("[lxmf-cam] native outbound too large bytes=%u max=%u\n",
                  (unsigned)payload_len,
                  (unsigned)BLE_NATIVE_WIRE_MAX_BYTES);
    g_diag.drop_backpressure++;
    return false;
  }
  uint8_t frame[1 + BLE_NATIVE_WIRE_MAX_BYTES];
  frame[0] = FRAME_NATIVE_WIRE;
  memcpy(&frame[1], payload, payload_len);
  notify_bytes(frame, payload_len + 1);
  return true;
}

static const char* fallback_reason_name(FallbackReasonCode code) {
  switch (code) {
    case FALLBACK_NONE:
      return "none";
    case FALLBACK_ACK_TIMEOUT:
      return "ack_timeout";
    case FALLBACK_CAMERA_ERROR:
      return "camera_error";
    case FALLBACK_QUEUE_BACKPRESSURE:
      return "queue_backpressure";
    default:
      return "unknown";
  }
}

static void notify_chunk(uint16_t seq, uint16_t total, const uint8_t* payload, uint16_t payload_len) {
  const uint32_t crc32 = 0;
  uint8_t frame[15 + 32];
  if (payload_len > 32) {
    notify_error("payload_too_large");
    return;
  }

  frame[0] = FRAME_CHUNK;
  frame[1] = (uint8_t)(g_transfer_id & 0xFF);
  frame[2] = (uint8_t)((g_transfer_id >> 8) & 0xFF);
  frame[3] = (uint8_t)((g_transfer_id >> 16) & 0xFF);
  frame[4] = (uint8_t)((g_transfer_id >> 24) & 0xFF);
  frame[5] = (uint8_t)(seq & 0xFF);
  frame[6] = (uint8_t)((seq >> 8) & 0xFF);
  frame[7] = (uint8_t)(total & 0xFF);
  frame[8] = (uint8_t)((total >> 8) & 0xFF);
  frame[9] = (uint8_t)(payload_len & 0xFF);
  frame[10] = (uint8_t)((payload_len >> 8) & 0xFF);
  frame[11] = (uint8_t)(crc32 & 0xFF);
  frame[12] = (uint8_t)((crc32 >> 8) & 0xFF);
  frame[13] = (uint8_t)((crc32 >> 16) & 0xFF);
  frame[14] = (uint8_t)((crc32 >> 24) & 0xFF);
  memcpy(&frame[15], payload, payload_len);

  notify_bytes(frame, 15 + payload_len);
}

class CaptureWriteCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    std::string value = characteristic->getValue();
    if (value.empty()) {
      return;
    }

    uint8_t frame_type = (uint8_t)value[0];
    if (frame_type == FRAME_CAPTURE_REQ) {
      g_capture_requested = true;
      g_diag.message_rx++;
      return;
    }

    if (frame_type == FRAME_NATIVE_ANNOUNCE_REQ) {
      g_diag.announce_sent++;
      Serial.println("[lxmf-cam] native announce requested");
      native_runtime_bridge_queue_message(NATIVE_DESTINATION_STUB, (const uint8_t*)"announce", 8);
      return;
    }

    if (frame_type == FRAME_NATIVE_MESSAGE_TX_REQ) {
      g_diag.message_tx++;
      Serial.println("[lxmf-cam] native message tx requested");
      if (value.size() > 1) {
        native_runtime_bridge_queue_message(
            NATIVE_DESTINATION_STUB,
            reinterpret_cast<const uint8_t*>(value.data() + 1),
            value.size() - 1);
      }
      return;
    }

    if (frame_type == FRAME_NATIVE_WIRE) {
      if (value.size() <= 1) {
        g_diag.drop_invalid++;
        Serial.println("[lxmf-cam] native inbound empty");
        return;
      }
      bool accepted = native_runtime_bridge_push_inbound_wire(
          reinterpret_cast<const uint8_t*>(value.data() + 1),
          value.size() - 1);
      if (accepted) {
        Serial.printf("[lxmf-cam] native inbound bytes=%u backend=%s\n",
                      (unsigned)(value.size() - 1),
                      native_runtime_bridge_backend_name());
      } else {
        g_diag.drop_invalid++;
        Serial.printf("[lxmf-cam] native inbound rejected bytes=%u backend=%s\n",
                      (unsigned)(value.size() - 1),
                      native_runtime_bridge_backend_name());
      }
      return;
    }

    if (frame_type == FRAME_CHUNK_ACK || frame_type == FRAME_NACK) {
      if (value.size() >= 7) {
        uint32_t ack_transfer_id = (uint32_t)(uint8_t)value[1]
            | ((uint32_t)(uint8_t)value[2] << 8)
            | ((uint32_t)(uint8_t)value[3] << 16)
            | ((uint32_t)(uint8_t)value[4] << 24);
        uint16_t ack_seq = (uint16_t)(uint8_t)value[5]
            | ((uint16_t)(uint8_t)value[6] << 8);
        if (!enqueue_ack_event(frame_type, ack_transfer_id, ack_seq)) {
          Serial.printf("[lxmf-cam] ack queue overflow transfer_id=%lu seq=%u type=0x%02x\n",
                        (unsigned long)ack_transfer_id,
                        (unsigned)ack_seq,
                        (unsigned)frame_type);
        }
      }
      if (frame_type == FRAME_NACK) {
        g_diag.drop_seq_gap++;
      }
      return;
    }

    g_diag.drop_invalid++;
  }
};

static bool wait_for_ack(uint32_t transfer_id, uint16_t seq, uint32_t timeout_ms) {
  uint32_t start = millis();
  while ((millis() - start) < timeout_ms) {
    AckEvent event;
    if (dequeue_ack_event(&event)) {
      if (event.transfer_id != transfer_id) {
        Serial.printf("[lxmf-cam] ack ignored transfer_id=%lu got_transfer_id=%lu seq=%u type=0x%02x\n",
                      (unsigned long)transfer_id,
                      (unsigned long)event.transfer_id,
                      (unsigned)event.seq,
                      (unsigned)event.type);
        continue;
      }
      if (event.type == FRAME_CHUNK_ACK && event.seq >= seq) {
        return true;
      }
      if (event.type == FRAME_CHUNK_ACK && event.seq < seq) {
        if (event.seq < 8 || ((event.seq + 1) % 100) == 0) {
          Serial.printf("[lxmf-cam] ack progress transfer_id=%lu ack_seq=%u wait_seq=%u\n",
                        (unsigned long)transfer_id,
                        (unsigned)event.seq,
                        (unsigned)seq);
        }
        continue;
      }
      if (event.type == FRAME_NACK) {
        Serial.printf("[lxmf-cam] nack transfer_id=%lu seq=%u\n",
                      (unsigned long)transfer_id,
                      (unsigned)event.seq);
        return false;
      }
      Serial.printf("[lxmf-cam] ack mismatch transfer_id=%lu seq=%u got_seq=%u type=0x%02x\n",
                    (unsigned long)transfer_id,
                    (unsigned)seq,
                    (unsigned)event.seq,
                    (unsigned)event.type);
    }
    delay(2);
  }
  return false;
}

static bool send_framebuffer_chunked(const uint8_t* data, size_t len) {
  if (len == 0) {
    notify_error("empty_frame");
    return false;
  }
  const uint32_t total_chunks_32 = (uint32_t)((len + BLE_CHUNK_PAYLOAD_BYTES - 1) / BLE_CHUNK_PAYLOAD_BYTES);
  if (total_chunks_32 > 65535U) {
    notify_error("frame_too_large");
    return false;
  }
  uint16_t total_chunks = (uint16_t)total_chunks_32;
  Serial.printf("[lxmf-cam] transfer start transfer_id=%lu frame_bytes=%u chunk_payload=%u total_chunks=%u\n",
                (unsigned long)g_transfer_id,
                (unsigned)len,
                (unsigned)BLE_CHUNK_PAYLOAD_BYTES,
                (unsigned)total_chunks);
  uint16_t seq = 0;
  size_t offset = 0;
  uint32_t transfer_started = millis();
  while (offset < len) {
    size_t batch_offset = offset;
    uint16_t batch_seq_start = seq;
    uint8_t batch_count = 0;
    while (offset < len && batch_count < BLE_ACK_EVERY_CHUNKS) {
      uint16_t n = (uint16_t)min((size_t)BLE_CHUNK_PAYLOAD_BYTES, len - offset);
      offset += n;
      seq++;
      batch_count++;
    }
    uint16_t batch_seq_end = (uint16_t)(seq - 1);
    size_t batch_len = offset - batch_offset;
    bool accepted = false;
    for (int attempt = 0; attempt < 3 && !accepted; ++attempt) {
      if (attempt > 0) {
        Serial.printf("[lxmf-cam] retry transfer_id=%lu seq_range=%u-%u attempt=%d offset=%u batch_bytes=%u\n",
                      (unsigned long)g_transfer_id,
                      (unsigned)batch_seq_start,
                      (unsigned)batch_seq_end,
                      attempt + 1,
                      (unsigned)batch_offset,
                      (unsigned)batch_len);
      }
      size_t resend_offset = batch_offset;
      uint16_t resend_seq = batch_seq_start;
      while (resend_offset < offset) {
        uint16_t n = (uint16_t)min((size_t)BLE_CHUNK_PAYLOAD_BYTES, offset - resend_offset);
        notify_chunk(resend_seq, total_chunks, data + resend_offset, n);
        resend_offset += n;
        resend_seq++;
      }
      accepted = wait_for_ack(g_transfer_id, batch_seq_end, BLE_ACK_TIMEOUT_MS);
      if (!accepted && attempt < 2) {
        g_diag.chunk_retry_total++;
      }
    }
    if (!accepted) {
      g_diag.drop_disconnected++;
      g_diag.fallback_reason = FALLBACK_ACK_TIMEOUT;
      Serial.printf("[lxmf-cam] transfer timeout transfer_id=%lu seq=%u offset=%u elapsed_ms=%lu\n",
                    (unsigned long)g_transfer_id,
                    (unsigned)batch_seq_end,
                    (unsigned)batch_offset,
                    (unsigned long)(millis() - transfer_started));
      notify_error("ack_timeout");
      return false;
    }
    if (batch_seq_end < 8 || ((batch_seq_end + 1) % 100) == 0 || offset >= len) {
      Serial.printf("[lxmf-cam] transfer progress transfer_id=%lu seq=%u/%u bytes_sent=%u/%u elapsed_ms=%lu\n",
                    (unsigned long)g_transfer_id,
                    (unsigned)batch_seq_end,
                    (unsigned)(total_chunks - 1),
                    (unsigned)offset,
                    (unsigned)len,
                    (unsigned long)(millis() - transfer_started));
    }
  }
  Serial.printf("[lxmf-cam] transfer complete transfer_id=%lu total_chunks=%u bytes=%u elapsed_ms=%lu\n",
                (unsigned long)g_transfer_id,
                (unsigned)total_chunks,
                (unsigned)len,
                (unsigned long)(millis() - transfer_started));
  return true;
}

static bool init_camera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 5;
  config.pin_d1 = 18;
  config.pin_d2 = 19;
  config.pin_d3 = 21;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;
  config.pin_xclk = 0;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_sccb_sda = 26;
  config.pin_sccb_scl = 27;
  config.pin_pwdn = 32;
  config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  const bool has_psram = psramFound();
  switch (g_node_config.capture_profile) {
    case NODE_CAPTURE_PROFILE_HIGH:
      config.frame_size = has_psram ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
      config.jpeg_quality = has_psram ? 14 : 16;
      config.fb_count = has_psram ? 2 : 1;
      break;
    case NODE_CAPTURE_PROFILE_BALANCED:
      config.frame_size = FRAMESIZE_QVGA;
      config.jpeg_quality = has_psram ? 16 : 18;
      config.fb_count = has_psram ? 2 : 1;
      break;
    case NODE_CAPTURE_PROFILE_THUMBNAIL:
    default:
      config.frame_size = FRAMESIZE_QQVGA;
      config.jpeg_quality = has_psram ? 20 : 22;
      config.fb_count = has_psram ? 2 : 1;
      break;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[lxmf-cam] camera init failed: 0x%x\n", err);
    return false;
  }
  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor != nullptr) {
    sensor->set_brightness(sensor, 0);
    sensor->set_saturation(sensor, 0);
  }
  lxmf_log_eventf("capture",
                  "camera_ready",
                  "camera ready profile=%s frame_size=%u jpeg_quality=%u fb_count=%u psram=%s",
                  node_runtime_capture_profile_name(g_node_config),
                  static_cast<unsigned>(config.frame_size),
                  static_cast<unsigned>(config.jpeg_quality),
                  static_cast<unsigned>(config.fb_count),
                  has_psram ? "yes" : "no");
  return true;
}

static void run_capture() {
  notify_capture_ack();
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    g_diag.drop_invalid++;
    g_diag.fallback_reason = FALLBACK_CAMERA_ERROR;
    notify_error("capture_failed");
    return;
  }
  Serial.printf("[lxmf-cam] frame bytes=%u\n", (unsigned)fb->len);
  bool ok = send_framebuffer_chunked(fb->buf, fb->len);
  esp_camera_fb_return(fb);
  if (ok) {
    notify_done();
    g_diag.message_tx++;
    g_transfer_id++;
  }
}

static void run_tcp_capture() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    g_diag.drop_invalid++;
    g_diag.fallback_reason = FALLBACK_CAMERA_ERROR;
    tcp_node_client_send_capture_result(0x02, 0, 0, 0, 0);
    lxmf_log_eventf("capture", "tcp_capture_failed", "tcp capture failed");
    return;
  }
  lxmf_log_eventf("capture",
                  "tcp_capture_start",
                  "tcp capture frame_bytes=%u width=%u height=%u",
                  (unsigned)fb->len,
                  (unsigned)fb->width,
                  (unsigned)fb->height);
  bool ok = tcp_node_client_send_capture(fb->buf, fb->len, fb->width, fb->height);
  esp_camera_fb_return(fb);
  if (ok) {
    g_diag.message_tx++;
    lxmf_log_eventf("capture", "tcp_capture_done", "tcp capture sent");
  } else {
    g_diag.drop_disconnected++;
    lxmf_log_eventf("capture", "tcp_capture_send_failed", "tcp capture send failed");
  }
}

static void native_node_runtime_tick() {
  if (!g_native_node_enabled) {
    return;
  }
  uint32_t now = millis();
  native_runtime_bridge_tick(now);

  if (g_node_config.node_mode != NATIVE_NODE_MODE_BLE_ONLY) {
    NativeRuntimeBridgeStats stats = native_runtime_bridge_stats();
    if (stats.outbound_frames > g_diag.announce_sent) {
      g_diag.announce_sent = stats.outbound_frames;
    }
    return;
  }

  uint8_t outbound[320];
  size_t outbound_len = 0;
  if (native_runtime_bridge_take_outbound_wire(outbound, sizeof(outbound), &outbound_len)) {
    bool emitted = false;
    if (g_connected) {
      emitted = notify_native_wire(outbound, outbound_len);
    }
    Serial.printf("[lxmf-cam] native outbound bytes=%u backend=%s emitted=%s\n",
                  (unsigned)outbound_len,
                  native_runtime_bridge_backend_name(),
                  emitted ? "yes" : "no");
  }

  NativeRuntimeBridgeStats stats = native_runtime_bridge_stats();
  if (stats.outbound_frames > g_diag.announce_sent) {
    g_diag.announce_sent = stats.outbound_frames;
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  lxmf_log_init(&Serial);
  lxmf_log_eventf("node", "boot", "boot");
  node_runtime_config_load(&g_node_config);
  native_runtime_bridge_init(&Serial);
  native_runtime_bridge_set_node_mode(g_node_config.node_mode);
  native_runtime_bridge_set_network_provisioned(node_runtime_config_has_wifi(g_node_config));
  native_runtime_bridge_set_ble_recovery_active(false);
  tcp_node_client_init(&Serial, &g_node_config);
  lxmf_log_eventf("node",
                  "config",
                  "config mode=%s wifi=%s tcp_host=%s tcp_port=%u",
                  node_runtime_config_mode_name(g_node_config),
                  node_runtime_config_has_wifi(g_node_config) ? "set" : "unset",
                  node_runtime_config_has_tcp_client_target(g_node_config) ? g_node_config.tcp_host : "<unset>",
                  g_node_config.tcp_port);

  g_ble_transport_enabled = (g_node_config.node_mode == NATIVE_NODE_MODE_BLE_ONLY);
  if (g_ble_transport_enabled) {
    BLEDevice::init(DEVICE_NAME);
    BLEServer* server = BLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());
    BLEService* service = server->createService(SERVICE_UUID);

    BLECharacteristic* write_char = service->createCharacteristic(
        WRITE_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    g_notify = service->createCharacteristic(
        NOTIFY_CHAR_UUID,
        BLECharacteristic::PROPERTY_NOTIFY);

    g_notify->addDescriptor(new BLE2902());
    write_char->setCallbacks(new CaptureWriteCallbacks());

    service->start();

    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(SERVICE_UUID);
    BLEAdvertisementData scan_data;
    std::string manufacturer_payload(MANUFACTURER_MARKER);
    scan_data.setManufacturerData(manufacturer_payload);
    advertising->setScanResponseData(scan_data);
    advertising->setScanResponse(true);
    advertising->setMinPreferred(0x06);
    advertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();

    lxmf_log_eventf("ble", "advertising", "advertising name=%s service=%s write=%s notify=%s manufacturer_marker=%s",
                    DEVICE_NAME, SERVICE_UUID, WRITE_CHAR_UUID, NOTIFY_CHAR_UUID, MANUFACTURER_MARKER);
    const uint8_t* mac = esp_bt_dev_get_address();
    if (mac != nullptr) {
      lxmf_log_eventf("ble",
                      "mac",
                      "ble_mac=%02X:%02X:%02X:%02X:%02X:%02X",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
  } else {
    lxmf_log_eventf("ble", "disabled", "disabled for tcp node mode");
  }
  init_camera();
}

void loop() {
  static uint32_t last_heartbeat = 0;
  static uint32_t last_diag = 0;
  uint32_t now = millis();
  if (now - last_heartbeat >= 5000) {
    last_heartbeat = now;
    lxmf_log_eventf("capture",
                    "heartbeat",
                    "heartbeat connected=%s transfer_id=%lu",
                    g_connected ? "yes" : "no",
                    (unsigned long)g_transfer_id);
  }
  if (now - last_diag >= 10000) {
    last_diag = now;
    NativeRuntimeBridgeStats native_stats = native_runtime_bridge_stats();
    TcpNodeClientStats tcp_stats = tcp_node_client_stats();
    lxmf_log_eventf(
        "capture",
        "diag",
        "diag announce=%lu msg_tx=%lu msg_rx=%lu retry=%lu drop_invalid=%lu drop_seq=%lu drop_disc=%lu drop_backpressure=%lu fallback=%s native_backend=%s native_ticks=%lu native_out=%lu native_in=%lu native_seq=%lu",
        (unsigned long)g_diag.announce_sent,
        (unsigned long)g_diag.message_tx,
        (unsigned long)g_diag.message_rx,
        (unsigned long)g_diag.chunk_retry_total,
        (unsigned long)g_diag.drop_invalid,
        (unsigned long)g_diag.drop_seq_gap,
        (unsigned long)g_diag.drop_disconnected,
        (unsigned long)g_diag.drop_backpressure,
        fallback_reason_name(g_diag.fallback_reason),
        native_runtime_bridge_backend_name(),
        (unsigned long)native_stats.ticks,
        (unsigned long)native_stats.outbound_frames,
        (unsigned long)native_stats.inbound_frames,
        (unsigned long)native_stats.last_sequence);
    lxmf_log_eventf(
        "native",
        "state",
        "state mode=%s lifecycle=%s provisioned=%s ble_recovery=%s",
        native_runtime_bridge_mode_name(),
        native_runtime_bridge_lifecycle_name(),
        native_stats.network_provisioned ? "yes" : "no",
        native_stats.ble_recovery_active ? "yes" : "no");
    lxmf_log_eventf(
        "net",
        "diag",
        "wifi=%s tcp=%s wifi_status=%u wifi_attempts=%lu tcp_attempts=%lu reconnects=%lu tx_frames=%lu rx_frames=%lu",
        tcp_stats.wifi_connected ? "up" : "down",
        tcp_stats.tcp_connected ? "up" : "down",
        (unsigned)tcp_stats.wifi_status,
        (unsigned long)tcp_stats.wifi_connect_attempts,
        (unsigned long)tcp_stats.tcp_connect_attempts,
        (unsigned long)tcp_stats.reconnects,
        (unsigned long)tcp_stats.tx_frames,
        (unsigned long)tcp_stats.rx_frames);
  }
  tcp_node_client_tick(now);
  native_node_runtime_tick();
  if (tcp_node_client_take_capture_request()) {
    lxmf_log_eventf("capture", "tcp_capture_requested", "tcp capture requested");
    run_tcp_capture();
  }
  if (g_capture_requested) {
    g_capture_requested = false;
    lxmf_log_eventf("capture", "ble_capture_requested", "ble capture requested");
    run_capture();
  }
  delay(20);
}
