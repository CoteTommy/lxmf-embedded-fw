#include "node_runtime_config.h"

#include <Preferences.h>
#include <string.h>

namespace {

constexpr const char* PREF_NAMESPACE = "lxmfnode";

const char* compile_time_str(const char* value) {
  return value != nullptr ? value : "";
}

NativeNodeMode default_mode() {
#ifdef LXMF_NODE_MODE_TCP_CLIENT
  return NATIVE_NODE_MODE_TCP_CLIENT;
#elif defined(LXMF_NODE_MODE_TCP_SERVER)
  return NATIVE_NODE_MODE_TCP_SERVER;
#else
  return NATIVE_NODE_MODE_BLE_ONLY;
#endif
}

NodeCaptureProfile default_capture_profile() {
#ifdef LXMF_CAPTURE_PROFILE_VERY_HIGH
  return NODE_CAPTURE_PROFILE_VERY_HIGH;
#elif defined(LXMF_CAPTURE_PROFILE_HIGH)
  return NODE_CAPTURE_PROFILE_HIGH;
#elif defined(LXMF_CAPTURE_PROFILE_BALANCED)
  return NODE_CAPTURE_PROFILE_BALANCED;
#else
  return NODE_CAPTURE_PROFILE_HIGH;
#endif
}

void apply_compile_time_defaults(NodeRuntimeConfig* config) {
  config->node_mode = default_mode();
  config->capture_profile = default_capture_profile();
#ifdef LXMF_WIFI_SSID
  strlcpy(config->wifi_ssid, compile_time_str(LXMF_WIFI_SSID), sizeof(config->wifi_ssid));
#endif
#ifdef LXMF_WIFI_PASSWORD
  strlcpy(config->wifi_password, compile_time_str(LXMF_WIFI_PASSWORD), sizeof(config->wifi_password));
#endif
#ifdef LXMF_TCP_HOST
  strlcpy(config->tcp_host, compile_time_str(LXMF_TCP_HOST), sizeof(config->tcp_host));
#endif
#ifdef LXMF_TCP_PORT
  config->tcp_port = LXMF_TCP_PORT;
#endif
}

}  // namespace

bool node_runtime_config_load(NodeRuntimeConfig* config) {
  if (config == nullptr) {
    return false;
  }

  *config = NodeRuntimeConfig{};

  Preferences prefs;
  if (!prefs.begin(PREF_NAMESPACE, true)) {
    apply_compile_time_defaults(config);
    return false;
  }

  config->node_mode =
      static_cast<NativeNodeMode>(prefs.getUChar("mode", static_cast<uint8_t>(config->node_mode)));
  config->ble_recovery_enabled = prefs.getBool("ble_recovery", config->ble_recovery_enabled);
  prefs.getString("wifi_ssid", config->wifi_ssid, sizeof(config->wifi_ssid));
  prefs.getString("wifi_password", config->wifi_password, sizeof(config->wifi_password));
  prefs.getString("tcp_host", config->tcp_host, sizeof(config->tcp_host));
  config->tcp_port = prefs.getUShort("tcp_port", config->tcp_port);
  config->capture_profile =
      static_cast<NodeCaptureProfile>(prefs.getUChar("capture_profile", static_cast<uint8_t>(config->capture_profile)));
  prefs.end();
  apply_compile_time_defaults(config);
  return true;
}

bool node_runtime_config_save(const NodeRuntimeConfig& config) {
  Preferences prefs;
  if (!prefs.begin(PREF_NAMESPACE, false)) {
    return false;
  }
  prefs.putUChar("mode", static_cast<uint8_t>(config.node_mode));
  prefs.putBool("ble_recovery", config.ble_recovery_enabled);
  prefs.putString("wifi_ssid", config.wifi_ssid);
  prefs.putString("wifi_password", config.wifi_password);
  prefs.putString("tcp_host", config.tcp_host);
  prefs.putUShort("tcp_port", config.tcp_port);
  prefs.putUChar("capture_profile", static_cast<uint8_t>(config.capture_profile));
  prefs.end();
  return true;
}

bool node_runtime_config_has_wifi(const NodeRuntimeConfig& config) {
  return config.wifi_ssid[0] != '\0';
}

bool node_runtime_config_has_tcp_client_target(const NodeRuntimeConfig& config) {
  return config.tcp_host[0] != '\0' && config.tcp_port != 0;
}

const char* node_runtime_config_mode_name(const NodeRuntimeConfig& config) {
  switch (config.node_mode) {
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

const char* node_runtime_capture_profile_name(const NodeRuntimeConfig& config) {
  switch (config.capture_profile) {
    case NODE_CAPTURE_PROFILE_THUMBNAIL:
      return "thumbnail";
    case NODE_CAPTURE_PROFILE_BALANCED:
      return "balanced";
    case NODE_CAPTURE_PROFILE_HIGH:
      return "high";
    case NODE_CAPTURE_PROFILE_VERY_HIGH:
      return "very_high";
    default:
      return "unknown";
  }
}
