#include "thermostat.hpp"
#include "logger.h"

// This can be turned off and on from the telnet session with the "Monitor" command
static bool telnet_logging_active = false;
static bool sdcard_logging_active = false;

static esp_log_level_t console_log_level = ESP_LOG_INFO;
static esp_log_level_t sdcard_log_level = ESP_LOG_WARN;
static esp_log_level_t telnet_log_level = ESP_LOG_INFO;

static vprintf_like_t OrigEsplogger = NULL;

bool enableTelnetLogging(bool enabled)
{
  telnet_logging_active = enabled;
  return telnet_logging_active;
}

bool enableSdcardLogging(bool enabled)
{
  sdcard_logging_active = enabled;
  return sdcard_logging_active;
}

void setConsoleLogLevel(esp_log_level_t level)
{
  console_log_level = level;
}

void setSdCardLogLevel(esp_log_level_t level)
{
  sdcard_log_level = level;
}

void setTelnetLogLevel(esp_log_level_t level)
{
  telnet_log_level = level;
}

esp_log_level_t map_char_to_level(char c)
{
  switch (c)
  {
    case 'E':
    case 'e':
      return ESP_LOG_ERROR;
    case 'W':
    case 'w':
      return ESP_LOG_WARN;
    case 'I':
    case 'i':
      return ESP_LOG_INFO;
    case 'D':
    case 'd':
      return ESP_LOG_DEBUG;
    default:
      return ESP_LOG_VERBOSE;
  }
}

int localLogger(const char *fmt, va_list list)
{
  int ret = 0;
  const char *prefix = fmt;
  if (prefix[0] == 0x1b /* ESC */)
  {
    const char *end = strchr(prefix, 'm');
    if (end != NULL)
      prefix = end + 1;
  }

  // CHIP and the Wi-Fi driver send message bodies without an ESP-IDF prefix.
  // Their callers already filter by severity; interpreting the first letter
  // of the message as a level discards valid messages (e.g. "Device state...").
  bool hasLevel = strchr("EWIDV", prefix[0]) != NULL && prefix[0] != '\0' &&
                  prefix[1] == ' ' && prefix[2] == '(';
  esp_log_level_t level = hasLevel ? map_char_to_level(prefix[0]) : ESP_LOG_NONE;
  auto writeTo = [&](vprintf_like_t sink, esp_log_level_t threshold) {
    if (sink == NULL || threshold == ESP_LOG_NONE || (hasLevel && level > threshold))
      return;
    // Each sink consumes its own argument list; do not share it across sinks.
    va_list copy;
    va_copy(copy, list);
    ret = sink(fmt, copy);
    va_end(copy);
  };

#ifdef TELNET_ENABLED
  if (telnet_logging_active)
    writeTo(telnet_esp32_vprintf, telnet_log_level);
#endif
  if (sdcard_logging_active)
    writeTo(sdcard_esp32_vprintf, sdcard_log_level);
  writeTo(OrigEsplogger, console_log_level);

  return ret;
}

void loggerEnableLocalLogging()
{
  if (OrigEsplogger == NULL)
    OrigEsplogger = esp_log_set_vprintf(localLogger);
}
