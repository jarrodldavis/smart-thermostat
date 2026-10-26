#pragma once
#include <cstdarg>

enum esp_log_level_t {
  ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE
};
using vprintf_like_t = int (*)(const char *, va_list);
vprintf_like_t esp_log_set_vprintf(vprintf_like_t callback);
