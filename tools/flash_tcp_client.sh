#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_DIR}"

if [[ -f ".env.local" ]]; then
  set -a
  # shellcheck disable=SC1091
  source ".env.local"
  set +a
fi

: "${LXMF_NODE_MODE_TCP_CLIENT:=1}"
: "${LXMF_TCP_PORT:=7443}"
: "${LXMF_CAPTURE_PROFILE:=high}"
: "${LXMF_RUST_FFI_LIB:=../LXMF-rs/target/xtensa-esp32-espidf/release/librns_embedded_ffi.a}"
: "${LXMF_RUST_FFI_INCLUDE:=../LXMF-rs/crates/libs/rns-embedded-ffi/include}"

required=(
  LXMF_NODE_MODE_TCP_CLIENT
  LXMF_WIFI_SSID
  LXMF_WIFI_PASSWORD
  LXMF_TCP_HOST
  LXMF_TCP_PORT
  LXMF_RUST_FFI_LIB
  LXMF_RUST_FFI_INCLUDE
)

missing=()
for name in "${required[@]}"; do
  if [[ -z "${!name:-}" ]]; then
    missing+=("${name}")
  fi
done

if (( ${#missing[@]} > 0 )); then
  printf 'Missing required environment values:\n' >&2
  printf '  %s\n' "${missing[@]}" >&2
  printf '\nCreate .env.local from .env.example or export them before running.\n' >&2
  exit 1
fi

if [[ ! "${LXMF_TCP_PORT}" =~ ^[0-9]+$ ]]; then
  printf 'Invalid LXMF_TCP_PORT=%q\n' "${LXMF_TCP_PORT}" >&2
  printf 'Expected a decimal port number. Check .env.local for a missing newline between LXMF_TCP_PORT and LXMF_CAPTURE_PROFILE.\n' >&2
  exit 1
fi

case "${LXMF_CAPTURE_PROFILE}" in
  thumbnail|balanced|high|very_high) ;;
  *)
    printf 'Invalid LXMF_CAPTURE_PROFILE=%q\n' "${LXMF_CAPTURE_PROFILE}" >&2
    printf 'Expected one of: thumbnail, balanced, high, very_high\n' >&2
    exit 1
    ;;
esac

echo "[flash-tcp-client] mode=tcp_client host=${LXMF_TCP_HOST} port=${LXMF_TCP_PORT} ssid=${LXMF_WIFI_SSID} profile=${LXMF_CAPTURE_PROFILE}"
echo "[flash-tcp-client] rust_ffi_lib=${LXMF_RUST_FFI_LIB}"

pio run -t clean
pio run -t upload
