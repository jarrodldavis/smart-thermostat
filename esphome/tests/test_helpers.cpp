// SPDX-License-Identifier: GPL-3.0-only
#include "../include/ui_temperature.h"
#include "../components/wifi_setup_password/password.h"
#include <cassert>
#include <cstdio>
#include <string>
using namespace smeisner;
void temperature_units() {
  auto close = [](float a, float b) { assert(std::fabs(a - b) < 0.0002f); };
  const auto absolute = TemperatureKind::ABSOLUTE;
  const auto difference = TemperatureKind::DIFFERENCE;
  close(to_display(0, true, absolute), 32);
  close(to_display(1, true, difference), 1.8f);
  close(to_display(-1, true, difference), -1.8f);
  close(from_display(32, true, absolute), 0);
  close(from_display(1.8f, true, difference), 1);
  close(to_display(30, true, TemperatureKind::NONE), 30);
  float c = 20;
  for (int n = 1; n <= 10; ++n) {
    c = adjust_display(c, 1, true, absolute, 0.5f, 1, 7, 35);
    close(to_display(c, true, absolute), 68.0f + n);
  }
  close(adjust_display(20, 1, false, absolute, 0.5f, 1, 7, 35), 20.5f);
  close(to_display(adjust_display(from_display(74.3f, true, absolute), -1, true,
    absolute, 0.5f, 1, 7, 35), true, absolute), 73);
  // Target-separation bounds must align with whole-degree Fahrenheit steps.
  c = adjust_display(from_display(70, true, absolute), 1, true, absolute, 0.5f, 1,
    7, from_display(74, true, absolute) - 2);
  close(to_display(c, true, absolute), 70);
  close(to_display(adjust_display(0, 1, true, absolute, 0.5f, 1, -40, 25), true, absolute), 33);
  close(to_display(adjust_display(0, 1, true, difference, 0.1f, 0.1f, -10, 10), true, difference), 0.1f);
  close(to_display(adjust_display(1, 1, true, difference, 0.5f, 0.1f, 0.5f, 10), true, difference), 1.9f);
  close(adjust_display(30, 1, true, TemperatureKind::NONE, 5, 1, 5, 300), 35);
}
void proof_of_possession() {
  uint8_t bytes[20]{};
  for (unsigned n = 0; n < 20; ++n) bytes[n] = n;
  const auto password = setup_password(bytes);
  assert(std::char_traits<char>::length(password.data()) == 20);
  assert(password[20] == '\0');
  assert(std::string(password.data()) == "ABCDEFGHJKLMNPQRSTUV");
  // Every byte value produces a printable, unambiguous WPA2 password character.
  for (unsigned value = 0; value < 256; ++value) {
    bytes[0] = value;
    const auto generated = setup_password(bytes);
    assert(std::string("ABCDEFGHJKLMNPQRSTUVWXYZ23456789").find(generated[0]) != std::string::npos);
  }
  bytes[0] = 31;
  assert(setup_password(bytes) != password);
}
int main() {
  proof_of_possession();
  temperature_units();
  std::puts("Display conversion and generated-password checks passed");
}
