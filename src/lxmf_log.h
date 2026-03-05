#pragma once

#include <Arduino.h>

enum LxmfLogMode : uint8_t {
  LXMF_LOG_MODE_TEXT = 0,
  LXMF_LOG_MODE_JSON = 1,
  LXMF_LOG_MODE_BOTH = 2,
};

void lxmf_log_init(Stream* stream);
LxmfLogMode lxmf_log_mode();
void lxmf_log_textf(const char* subsystem, const char* fmt, ...);
void lxmf_log_eventf(const char* subsystem, const char* event, const char* fmt, ...);
