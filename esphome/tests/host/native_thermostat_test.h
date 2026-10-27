#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstdint>
namespace native_test {
inline unsigned routing_evaluations{0}, before_wait{0};
inline void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "Native thermostat integration failed: %s\n", message);
    std::exit(1);
  }
}
inline bool pins[6]{};  // W, Y1, Y2, G, O/B, AUX; actual output writes, not desired state.
inline unsigned output_writes{0};
inline unsigned falling_edges[6]{};
inline uint32_t compressor_stopped{0}, stage2_stopped{0}, valve_changed{0}, backup_stopped{0};
inline uint32_t expected_pre_ms{1000}, expected_post_ms{1000};
inline bool compressor_was_stopped{false}, stage2_was_stopped{false}, valve_was_changed{false}, backup_was_stopped{false};
inline void record(unsigned pin, bool state, bool furnace, uint32_t now) {
  if (pins[pin] && !state) ++falling_edges[pin];
  if (pin == 1 && pins[pin] && !state) { compressor_stopped = now; compressor_was_stopped = true; }
  if (pin == 2 && pins[pin] && !state) { stage2_stopped = now; stage2_was_stopped = true; }
  if (pin == 4 && pins[pin] != state) {
    if (compressor_was_stopped) check(uint32_t(now - compressor_stopped) + 5 >= expected_pre_ms, "pre-change off interval before O/B command");
    valve_changed = now;
    valve_was_changed = true;
  }
  if (pin == 5 && pins[pin] && !state && !pins[1]) { backup_stopped = now; backup_was_stopped = true; }
  if (pin == 1 && !pins[pin] && state) {
    if (compressor_was_stopped) check(uint32_t(now - compressor_stopped) >= 1995, "physical compressor off interval");
    if (valve_was_changed) check(uint32_t(now - valve_changed) + 5 >= expected_post_ms, "valve settling before compressor start");
    if (backup_was_stopped) check(uint32_t(now - backup_stopped) >= 995, "backup settling before compressor return");
  }
  if (pin == 2 && !pins[pin] && state && stage2_was_stopped)
    check(uint32_t(now - stage2_stopped) >= 1995, "physical stage-two off interval");
  if (pin == 5 && !pins[pin] && state && furnace && compressor_was_stopped)
    check(uint32_t(now - compressor_stopped) >= 995, "furnace takeover all-off interval");
  if (pin == 4 && pins[pin] != state) check(!pins[1], "O/B never moves under an energized compressor");
  pins[pin] = state;
  ++output_writes;
  check(!pins[2] || pins[1], "physical Y2 requires physical Y1");
  check(!pins[0] || (!pins[1] && !pins[5]), "W excludes compressor and AUX");
  if (furnace) check(!pins[5] || (!pins[1] && !pins[2]), "furnace excludes both compressor stages");
}
}  // namespace native_test
