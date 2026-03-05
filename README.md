# lxmf-esp32-cam-fw

ESP32-CAM firmware scaffold for BLE camera-capture interoperability with `rnx camera-capture-upload`.

Current status:
- real OV2640 JPEG capture over BLE camera protocol
- native embedded runtime bridge integrated into firmware loop
- Rust FFI backend works when `rns-embedded-ffi` is built for `xtensa-esp32-espidf` and linked into the PlatformIO build
- bridge now tracks node mode, provisioning state, and runtime lifecycle so Wi-Fi/TCP node mode can be layered in without breaking the BLE proof path

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

## Log formats

Default serial logs use readable text lines.

Optional build flags in `platformio.ini`:

- `-DLXMF_LOG_FORMAT_JSON=1`
  - emit JSONL only
- `-DLXMF_LOG_FORMAT_BOTH=1`
  - emit text and JSONL for each migrated log event

Current structured logging coverage includes:

- node boot/config
- BLE mode/connection state
- native runtime init/state
- Wi-Fi/TCP connection lifecycle and diagnostics

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

## TCP client scaffold

Firmware now includes a first TCP client scaffold on top of the native runtime bridge:

- `src/node_runtime_config.h/.cpp`
  - persisted node mode / Wi-Fi / TCP endpoint config
- `src/tcp_node_client.h/.cpp`
  - Wi-Fi connect loop
  - TCP client reconnect loop
  - length-prefixed native runtime frame send/receive

Current configuration sources:

1. persisted `Preferences` namespace `lxmfnode`
2. optional environment-driven build defines applied by `tools/configure_rust_ffi.py`:
 - `LXMF_NODE_MODE_TCP_CLIENT=1`
 - `LXMF_WIFI_SSID=your-ssid`
 - `LXMF_WIFI_PASSWORD=your-password`
 - `LXMF_TCP_HOST=host-or-ip`
  - `LXMF_TCP_PORT=7443`
  - `LXMF_CAPTURE_PROFILE=thumbnail|balanced|high|very_high`

Recommended local workflow:

```bash
cp .env.example .env.local
```

Edit `.env.local` with your real values, then run:

```bash
./tools/flash_tcp_client.sh
```

Manual example:

```bash
LXMF_NODE_MODE_TCP_CLIENT=1 \
LXMF_WIFI_SSID="your-ssid" \
LXMF_WIFI_PASSWORD="your-password" \
LXMF_TCP_HOST="192.168.1.10" \
LXMF_TCP_PORT=7443 \
LXMF_RUST_FFI_LIB=../LXMF-rs/target/xtensa-esp32-espidf/release/librns_embedded_ffi.a \
LXMF_RUST_FFI_INCLUDE=../LXMF-rs/crates/libs/rns-embedded-ffi/include \
pio run -t upload
```

BLE is disabled in TCP node modes to preserve Wi-Fi memory headroom. It remains the provisioning/recovery transport in `ble_only` mode.

Capture profiles:

- `thumbnail`
  - reliability-first
  - `QQVGA`
- `balanced`
  - better image quality for normal TCP use
  - `QVGA`
- `high`
  - default profile
  - higher detail, larger transfers
  - `VGA` with PSRAM, `QVGA` otherwise
- `very_high`
  - pushes resolution/quality above the default
  - `SVGA` with PSRAM, `VGA` otherwise

TCP capture requests can also override the profile per request without reflashing. Host tooling now supports:

- `default`
- `thumbnail`
- `balanced`
- `high`
- `very_high`

## Next steps

1. Add Wi-Fi persistence and provisioning on top of the runtime lifecycle scaffold.
2. Wire TCP client mode into the bridge as the first standalone-node transport.
3. Add TCP server mode after client mode is stable on-device.
4. Converge camera transport with runtime attachment semantics and hash/CRC verification.
