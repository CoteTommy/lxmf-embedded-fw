#include "lxmf_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

Stream* g_stream = nullptr;

LxmfLogMode detect_mode() {
#if defined(LXMF_LOG_FORMAT_JSON)
  return LXMF_LOG_MODE_JSON;
#elif defined(LXMF_LOG_FORMAT_BOTH)
  return LXMF_LOG_MODE_BOTH;
#else
  return LXMF_LOG_MODE_TEXT;
#endif
}

void format_message(char* buf, size_t buf_len, const char* fmt, va_list args) {
  if (buf_len == 0) {
    return;
  }
  vsnprintf(buf, buf_len, fmt, args);
}

void write_text(const char* subsystem, const char* message) {
  if (g_stream == nullptr) {
    return;
  }
  g_stream->printf("[lxmf-%s] %s\n", subsystem, message);
}

void write_json(const char* subsystem, const char* event, const char* message) {
  if (g_stream == nullptr) {
    return;
  }
  char escaped[224];
  size_t out = 0;
  for (size_t i = 0; message[i] != '\0' && out + 2 < sizeof(escaped); ++i) {
    char c = message[i];
    if (c == '"' || c == '\\') {
      escaped[out++] = '\\';
      escaped[out++] = c;
    } else if (c == '\n' || c == '\r') {
      escaped[out++] = ' ';
    } else {
      escaped[out++] = c;
    }
  }
  escaped[out] = '\0';
  g_stream->printf(
      "{\"ts_ms\":%lu,\"subsystem\":\"%s\",\"event\":\"%s\",\"message\":\"%s\"}\n",
      (unsigned long)millis(),
      subsystem,
      event != nullptr ? event : "log",
      escaped);
}

void write_log(const char* subsystem, const char* event, const char* message) {
  switch (detect_mode()) {
    case LXMF_LOG_MODE_JSON:
      write_json(subsystem, event, message);
      break;
    case LXMF_LOG_MODE_BOTH:
      write_text(subsystem, message);
      write_json(subsystem, event, message);
      break;
    case LXMF_LOG_MODE_TEXT:
    default:
      write_text(subsystem, message);
      break;
  }
}

}  // namespace

void lxmf_log_init(Stream* stream) {
  g_stream = stream;
}

LxmfLogMode lxmf_log_mode() {
  return detect_mode();
}

void lxmf_log_textf(const char* subsystem, const char* fmt, ...) {
  char message[192];
  va_list args;
  va_start(args, fmt);
  format_message(message, sizeof(message), fmt, args);
  va_end(args);
  write_log(subsystem, "log", message);
}

void lxmf_log_eventf(const char* subsystem, const char* event, const char* fmt, ...) {
  char message[192];
  va_list args;
  va_start(args, fmt);
  format_message(message, sizeof(message), fmt, args);
  va_end(args);
  write_log(subsystem, event, message);
}
