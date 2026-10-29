// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "esphome/core/helpers.h"
#include <algorithm>
#include <cmath>

namespace smeisner {
enum class TemperatureKind { NONE, ABSOLUTE, DIFFERENCE };
inline float to_display(float c, bool fahrenheit, TemperatureKind kind) {
  if (!fahrenheit || kind == TemperatureKind::NONE) return c;
  return kind == TemperatureKind::ABSOLUTE ? esphome::celsius_to_fahrenheit(c) :
      esphome::celsius_to_fahrenheit(c) - esphome::celsius_to_fahrenheit(0.0f);
}
inline float from_display(float value, bool fahrenheit, TemperatureKind kind) {
  if (!fahrenheit || kind == TemperatureKind::NONE) return value;
  return kind == TemperatureKind::ABSOLUTE ? esphome::fahrenheit_to_celsius(value) :
      esphome::fahrenheit_to_celsius(value + esphome::celsius_to_fahrenheit(0.0f));
}
// Round the displayed value and bounds to the adjustment step so target
// separation cannot introduce fractional Fahrenheit setpoints at a limit.
inline float adjust_display(float c, int direction, bool fahrenheit, TemperatureKind kind,
                            float c_step, float f_step, float minimum_c, float maximum_c) {
  const float step = fahrenheit && kind != TemperatureKind::NONE ? f_step : c_step;
  if (!std::isfinite(c) || !std::isfinite(step) || step <= 0 || direction == 0) return c;
  constexpr float rounding_epsilon = 0.0001f;
  const float lower = std::ceil(to_display(minimum_c, fahrenheit, kind) / step - rounding_epsilon) * step;
  const float upper = std::floor(to_display(maximum_c, fahrenheit, kind) / step + rounding_epsilon) * step;
  if (lower > upper) return c;
  const float current = std::round(to_display(c, fahrenheit, kind) / step);
  const float next = (current + (direction > 0 ? 1 : -1)) * step;
  return std::clamp(from_display(std::clamp(next, lower, upper), fahrenheit, kind), minimum_c, maximum_c);
}
}  // namespace smeisner
