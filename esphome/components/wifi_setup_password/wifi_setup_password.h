// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "esphome/core/component.h"
#include <array>
#include <string>

namespace esphome::wifi_setup_password {
class WiFiSetupPassword final : public Component {
 public:
  float get_setup_priority() const override { return 1100.0f; }
  void setup() override;
  bool is_active() const;
  const char *password() const { return password_.data(); }
  const char *ssid() const { return ssid_.c_str(); }
 protected:
  std::array<char, 21> password_{};
  std::string ssid_;
};
}  // namespace esphome::wifi_setup_password
