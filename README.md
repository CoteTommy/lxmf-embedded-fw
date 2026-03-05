# lxmf-esp32-cam-fw

ESP32-CAM firmware scaffold for BLE camera-capture interoperability with `rnx camera-capture-upload`.

Current status:
- real OV2640 JPEG capture over BLE camera protocol
- native embedded runtime bridge integrated into firmware loop
- Rust FFI backend works when `rns-embedded-ffi` is built for `xtensa-esp32-espidf` and linked into the PlatformIO build

## Protocol

Implements the frame ids expected by current host bridge:

- `0x02` CAPTURE_REQ (host -> device write)
- `0x03` CAPTURE_ACK (device -> host notify)
- `0x04` CHUNK (device -> host notify)
- `0x05` CHUNK_ACK (host -> device write)
- `0x06` DONE (device -> host notify)
- `0x07` ERROR (device -> host notify)
- `0x08` NACK (host -> device write)
- `0x23` NATIVE_WIRE (bidirectional runtime frame wrapper)

CHUNK wire layout:

- byte 0: frame type (`0x04`)
- bytes 1..4: `transfer_id` (little-endian `u32`)
- bytes 5..6: `seq` (little-endian `u16`)
- bytes 7..8: `total_chunks` (little-endian `u16`)
- bytes 9..10: `payload_len` (little-endian `u16`)
- bytes 11..14: `crc32` (little-endian `u32`, currently `0` in stub)
- bytes 15..: payload

NATIVE_WIRE layout:

- byte 0: frame type (`0x23`)
- bytes 1..: encoded `rns-embedded-core` packet frame bytes (`RNE1...`)

Temporary helper control frames:

- `0x21` NATIVE_ANNOUNCE_REQ
  - host asks firmware to queue a native runtime announce through the on-device runtime
- `0x22` NATIVE_MESSAGE_TX_REQ
  - host asks firmware to queue a native runtime LXMF message body through the on-device runtime

These are convenience triggers for bring-up. The intended steady-state transport is `0x23`
carrying encoded native runtime frames directly.

## UUID defaults

- Service UUID: `12345678-1234-1234-1234-1234567890ab`
- Write char UUID: `12345678-1234-1234-1234-1234567890ac`
- Notify char UUID: `12345678-1234-1234-1234-1234567890ad`

Peripheral name: `LXMF-CAM-STUB`

## Build and flash

```bash
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

## Native runtime bridge

Firmware now includes `src/native_runtime_bridge.h/.cpp`, which is the seam for the
embedded Reticulum runtime.

- If `rns_embedded_ffi.h` and a matching Rust static library are available to the build,
  the bridge can call the Rust FFI node runtime.
- Without that toolchain, the bridge runs a stub backend and logs native tick activity so
  the firmware integration points remain exercised.

PlatformIO now auto-detects the Rust FFI library via `tools/configure_rust_ffi.py`.

It enables the real bridge when either:

1. these environment variables are set:
   - `LXMF_RUST_FFI_LIB=/absolute/path/to/librns_embedded_ffi.a`
   - `LXMF_RUST_FFI_INCLUDE=/absolute/path/to/rns-embedded-ffi/include`
2. or one of the common sibling repo artifacts exists:
   - `../LXMF-rs/target/xtensa-esp32-espidf/release/librns_embedded_ffi.a`
   - `../LXMF-rs/target/xtensa-esp32-none-elf/release/librns_embedded_ffi.a`
   - `../LXMF-rs/target/xtensa-esp32s3-none-elf/release/librns_embedded_ffi.a`

If no valid library/header pair is found, firmware logs and runs with `backend=stub`.

## Next steps

1. Expand the pure `0x23` native wire flow beyond test ping/LXMF ping into fuller host interop.
2. Bridge native BLE runtime traffic cleanly into standard host Reticulum services.
3. Populate `crc32` and converge the camera transport with runtime attachment semantics.
4. Add transport alternatives such as Wi-Fi/TCP once the runtime framing is stable.
