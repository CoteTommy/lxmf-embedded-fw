# lxmf-esp32-cam-fw

ESP32-CAM firmware scaffold for BLE camera-capture interoperability with `rnx camera-capture-upload`.

Current status: `ble_stub` implemented (fake chunk stream). No real camera capture yet.

## Protocol

Implements the frame ids expected by current host bridge:

- `0x02` CAPTURE_REQ (host -> device write)
- `0x03` CAPTURE_ACK (device -> host notify)
- `0x04` CHUNK (device -> host notify)
- `0x05` CHUNK_ACK (host -> device write)
- `0x06` DONE (device -> host notify)
- `0x07` ERROR (device -> host notify)
- `0x08` NACK (host -> device write)

CHUNK wire layout:

- byte 0: frame type (`0x04`)
- bytes 1..4: `transfer_id` (little-endian `u32`)
- bytes 5..6: `seq` (little-endian `u16`)
- bytes 7..8: `total_chunks` (little-endian `u16`)
- bytes 9..10: `payload_len` (little-endian `u16`)
- bytes 11..14: `crc32` (little-endian `u32`, currently `0` in stub)
- bytes 15..: payload

## UUID defaults

- Service UUID: `12345678-1234-1234-1234-1234567890ab`
- Write char UUID: `12345678-1234-1234-1234-1234567890ac`
- Notify char UUID: `12345678-1234-1234-1234-1234567890ad`

Peripheral name: `LXMF-CAM-STUB`

## Build and flash

```bash
cd /Users/tommy/Documents/TAK/lxmf-esp32-cam-fw
pio run
pio run -t upload
pio device monitor
```

## Host smoke command

After flash, run from `LXMF-rs` repo:

```bash
PERIPHERAL_ID="LXMF-CAM-STUB" \
SERVICE_UUID="12345678-1234-1234-1234-1234567890ab" \
WRITE_CHAR_UUID="12345678-1234-1234-1234-1234567890ac" \
NOTIFY_CHAR_UUID="12345678-1234-1234-1234-1234567890ad" \
./tools/scripts/esp32-camera-capture-smoke.sh
```

## Next steps

1. Replace stub bytes with real OV2640 JPEG capture.
2. Populate `crc32`.
3. Add chunk retry behavior on host `NACK`.
