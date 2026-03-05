#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

static const char* DEVICE_NAME = "LXMF-CAM-STUB";

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

static BLECharacteristic* g_notify = nullptr;
static volatile bool g_capture_requested = false;

static uint32_t g_transfer_id = 1;

static void notify_bytes(const uint8_t* data, size_t len) {
  if (g_notify == nullptr) {
    return;
  }
  g_notify->setValue(data, len);
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

static void notify_chunk(uint16_t seq, uint16_t total, const uint8_t* payload, uint16_t payload_len) {
  const uint32_t crc32 = 0;
  uint8_t frame[15 + 64];
  if (payload_len > 64) {
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
      return;
    }

    if (frame_type == FRAME_CHUNK_ACK || frame_type == FRAME_NACK) {
      // Stub currently ignores ACK/NACK details.
      return;
    }
  }
};

static void run_stub_capture() {
  // Fake payload split into 3 chunks.
  static const uint8_t c0[] = "FAKEJPEG_CHUNK_000_";
  static const uint8_t c1[] = "FAKEJPEG_CHUNK_001_";
  static const uint8_t c2[] = "FAKEJPEG_CHUNK_002_END";

  notify_capture_ack();
  notify_chunk(0, 3, c0, sizeof(c0) - 1);
  notify_chunk(1, 3, c1, sizeof(c1) - 1);
  notify_chunk(2, 3, c2, sizeof(c2) - 1);
  notify_done();

  g_transfer_id++;
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("[lxmf-cam] boot");

  BLEDevice::init(DEVICE_NAME);
  BLEServer* server = BLEDevice::createServer();
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
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("[lxmf-cam] advertising");
  Serial.println(DEVICE_NAME);
  Serial.println(SERVICE_UUID);
  Serial.println(WRITE_CHAR_UUID);
  Serial.println(NOTIFY_CHAR_UUID);
}

void loop() {
  if (g_capture_requested) {
    g_capture_requested = false;
    Serial.println("[lxmf-cam] capture requested");
    run_stub_capture();
  }
  delay(20);
}
