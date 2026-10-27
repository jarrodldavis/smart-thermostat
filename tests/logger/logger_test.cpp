#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include "logger.h"

void loggerEnableLocalLogging();

static std::vector<std::string> console, telnet, sdcard;

static int capture(std::vector<std::string> &sink, const char *fmt, va_list args)
{
  char buffer[256];
  int length = vsnprintf(buffer, sizeof(buffer), fmt, args);
  sink.emplace_back(buffer);
  return length;
}

static int consoleWrite(const char *fmt, va_list args) { return capture(console, fmt, args); }
int telnet_esp32_vprintf(const char *fmt, va_list args) { return capture(telnet, fmt, args); }
int sdcard_esp32_vprintf(const char *fmt, va_list args) { return capture(sdcard, fmt, args); }
vprintf_like_t esp_log_set_vprintf(vprintf_like_t) { return consoleWrite; }

static void log(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  localLogger(fmt, args);
  va_end(args);
}

static void clear() { console.clear(); telnet.clear(); sdcard.clear(); }

int main()
{
  loggerEnableLocalLogging();
  enableTelnetLogging(true);
  enableSdcardLogging(true);
  setConsoleLogLevel(ESP_LOG_INFO);
  setTelnetLogLevel(ESP_LOG_INFO);
  setSdCardLogLevel(ESP_LOG_INFO);

  // CHIP emits the prefix separately: the body's first letter is not severity.
  log("Device state: %d %s", 42, "connected");
  assert(console == std::vector<std::string>{"Device state: 42 connected"});
  assert(sdcard == console);
#ifdef TELNET_ENABLED
  assert(telnet == console);
#else
  assert(telnet.empty());
#endif

  // All sinks must receive a fresh va_list, including strings and numbers.
  clear();
  log("\033[0;31mE (%d) test: %s %d\033[0m\n", 123, "failure", 456);
  assert(console == std::vector<std::string>{"\033[0;31mE (123) test: failure 456\033[0m\n"});
  assert(sdcard == console);
#ifdef TELNET_ENABLED
  assert(telnet == console);
#else
  assert(telnet.empty());
#endif

  clear();
  log("D (%d) test: filtered", 123);
  assert(console.empty() && telnet.empty() && sdcard.empty());

  // Short/raw formats are valid too; no prefix indexing past their terminator.
  log("I");
  log("");
  assert(console == std::vector<std::string>({"I", ""}));

  clear();
  setConsoleLogLevel(ESP_LOG_NONE);
  enableTelnetLogging(false);
  enableSdcardLogging(false);
  log("Device state: %d", 42);
  assert(console.empty() && telnet.empty() && sdcard.empty());
  puts("logger regression checks passed");
}
