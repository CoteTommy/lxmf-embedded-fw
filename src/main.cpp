#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEAdvertising.h>
#include <esp_bt_device.h>
#include <esp_camera.h>

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

static BLECharacteristic* g_notify = nullptr;
static volatile bool g_capture_requested = false;
static volatile bool g_ack_pending = false;
static volatile uint8_t g_ack_type = 0;
static volatile uint32_t g_ack_transfer_id = 0;
static volatile uint16_t g_ack_seq = 0;

static uint32_t g_transfer_id = 1;
static bool g_connected = false;
static bool g_native_node_enabled = true;

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

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    (void)server;
    g_connected = true;
    Serial.println("[lxmf-cam] ble client connected");
  }

  void onDisconnect(BLEServer* server) override {
    (void)server;
    g_connected = false;
    Serial.println("[lxmf-cam] ble client disconnected");
    BLEDevice::startAdvertising();
    Serial.println("[lxmf-cam] advertising restarted");
  }
};

static void notify_bytes(const uint8_t* data, size_t len) {
  if (g_notify == nullptr) {
    return;
  }
  g_notify->setValue(const_cast<uint8_t*>(data), len);
  g_notify->notify();
  delay(12);
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
      return;
    }

    if (frame_type == FRAME_NATIVE_MESSAGE_TX_REQ) {
      g_diag.message_tx++;
      Serial.println("[lxmf-cam] native message tx requested");
      return;
    }

    if (frame_type == FRAME_CHUNK_ACK || frame_type == FRAME_NACK) {
      if (value.size() >= 7) {
        g_ack_type = frame_type;
        g_ack_transfer_id = (uint32_t)(uint8_t)value[1]
            | ((uint32_t)(uint8_t)value[2] << 8)
            | ((uint32_t)(uint8_t)value[3] << 16)
            | ((uint32_t)(uint8_t)value[4] << 24);
        g_ack_seq = (uint16_t)(uint8_t)value[5]
            | ((uint16_t)(uint8_t)value[6] << 8);
        g_ack_pending = true;
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
    if (g_ack_pending) {
      noInterrupts();
      bool pending = g_ack_pending;
      uint8_t ack_type = g_ack_type;
      uint32_t ack_transfer_id = g_ack_transfer_id;
      uint16_t ack_seq = g_ack_seq;
      g_ack_pending = false;
      interrupts();
      if (!pending) {
        continue;
      }
      if (ack_transfer_id != transfer_id) {
        continue;
      }
      if (ack_type == FRAME_CHUNK_ACK && ack_seq == seq) {
        return true;
      }
      if (ack_type == FRAME_NACK && ack_seq == seq) {
        return false;
      }
    }
    delay(2);
  }
  return false;
}

static bool send_framebuffer_chunked(const uint8_t* data, size_t len) {
  const uint16_t chunk_payload = 19;
  if (len == 0) {
    notify_error("empty_frame");
    return false;
  }
  const uint32_t total_chunks_32 = (uint32_t)((len + chunk_payload - 1) / chunk_payload);
  if (total_chunks_32 > 65535U) {
    notify_error("frame_too_large");
    return false;
  }
  uint16_t total_chunks = (uint16_t)total_chunks_32;
  uint16_t seq = 0;
  size_t offset = 0;
  while (offset < len) {
    uint16_t n = (uint16_t)min((size_t)chunk_payload, len - offset);
    bool accepted = false;
    for (int attempt = 0; attempt < 3 && !accepted; ++attempt) {
      notify_chunk(seq, total_chunks, data + offset, n);
      accepted = wait_for_ack(g_transfer_id, seq, 900);
      if (!accepted && attempt < 2) {
        g_diag.chunk_retry_total++;
      }
    }
    if (!accepted) {
      g_diag.drop_disconnected++;
      g_diag.fallback_reason = FALLBACK_ACK_TIMEOUT;
      notify_error("ack_timeout");
      return false;
    }
    offset += n;
    seq++;
  }
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

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 14;
    config.fb_count = 1;
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
  Serial.println("[lxmf-cam] camera ready");
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

static void native_node_runtime_tick() {
  // Integration hook for rns-embedded-core runtime:
  // - poll inbound transport frames
  // - advance announce/message scheduler
  // - persist replay/cursor state checkpoints
  if (!g_native_node_enabled) {
    return;
  }
  static uint32_t last_announce_ms = 0;
  uint32_t now = millis();
  if (g_connected && (now - last_announce_ms) >= 30000) {
    last_announce_ms = now;
    g_diag.announce_sent++;
    Serial.println("[lxmf-cam] native announce tick");
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("[lxmf-cam] boot");

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

  Serial.println("[lxmf-cam] advertising");
  Serial.println(DEVICE_NAME);
  Serial.println(SERVICE_UUID);
  Serial.println(WRITE_CHAR_UUID);
  Serial.println(NOTIFY_CHAR_UUID);
  Serial.print("manufacturer_marker=");
  Serial.println(MANUFACTURER_MARKER);
  const uint8_t* mac = esp_bt_dev_get_address();
  if (mac != nullptr) {
    Serial.printf("ble_mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
  init_camera();
}

void loop() {
  static uint32_t last_heartbeat = 0;
  static uint32_t last_diag = 0;
  uint32_t now = millis();
  if (now - last_heartbeat >= 5000) {
    last_heartbeat = now;
    Serial.printf("[lxmf-cam] heartbeat connected=%s transfer_id=%lu\n",
                  g_connected ? "yes" : "no",
                  (unsigned long)g_transfer_id);
  }
  if (now - last_diag >= 10000) {
    last_diag = now;
    Serial.printf(
        "[lxmf-cam] diag announce=%lu msg_tx=%lu msg_rx=%lu retry=%lu drop_invalid=%lu drop_seq=%lu drop_disc=%lu drop_backpressure=%lu fallback=%s\n",
        (unsigned long)g_diag.announce_sent,
        (unsigned long)g_diag.message_tx,
        (unsigned long)g_diag.message_rx,
        (unsigned long)g_diag.chunk_retry_total,
        (unsigned long)g_diag.drop_invalid,
        (unsigned long)g_diag.drop_seq_gap,
        (unsigned long)g_diag.drop_disconnected,
        (unsigned long)g_diag.drop_backpressure,
        fallback_reason_name(g_diag.fallback_reason));
  }
  native_node_runtime_tick();
  if (g_capture_requested) {
    g_capture_requested = false;
    Serial.println("[lxmf-cam] capture requested");
    run_capture();
  }
  delay(20);
}
