// SPDX-License-Identifier: GPL-3.0-only
#include "wifi_setup_password.h"
#include "password.h"
#include "esphome/core/application.h"
#include "esphome/components/wifi/wifi_component.h"
#include "bootloader_random.h"
#include "esp_random.h"

namespace esphome::wifi_setup_password {
void WiFiSetupPassword::setup() {
  // Espressif's early-application entropy source is safe here, before RF/ADC.
  uint8_t random[20];
  bootloader_random_enable();
  esp_fill_random(random, sizeof(random));
  bootloader_random_disable();
  password_ = smeisner::setup_password(random);

  auto *network = wifi::global_wifi_component;
  auto ap = network->get_ap();
  ssid_ = std::string(App.get_name().c_str());
  // Retain the MAC suffix when the device name exceeds the SSID limit.
  if (ssid_.size() > 26) ssid_ = ssid_.substr(0, 19) + ssid_.substr(ssid_.size() - 7);
  ssid_ += "-setup";
  ap.set_ssid(ssid_);
  ap.set_password(password_.data());
  network->set_ap(ap);
}

bool WiFiSetupPassword::is_active() const {
  return wifi::global_wifi_component->is_ap_active();
}
}  // namespace esphome::wifi_setup_password
