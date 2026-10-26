// SPDX-License-Identifier: GPL-3.0-only
#ifdef MATTER_ENABLED

#include "thermostat.hpp"
#include <esp_matter.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <platform/CHIPDeviceLayer.h>
#include "esp_heap_caps.h"

using namespace chip;
using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "Matter";

static void on_device_event(const ChipDeviceEvent *event, intptr_t arg)
{
  switch (event->Type)
  {
  case DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
    ESP_LOGI(TAG, "Interface IP address changed");
    WifiMatterStarted();
    break;
  case DeviceLayer::DeviceEventType::kWiFiConnectivityChange:
    ESP_LOGI(TAG, "Wi-Fi connectivity changed");
    WifiMatterStarted();
    break;
  case DeviceLayer::DeviceEventType::kCommissioningComplete:
    ESP_LOGI(TAG, "Commissioning complete");
    break;
  case DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
    ESP_LOGI(TAG, "Commissioning failed, fail-safe timer expired");
    break;
  case DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
    ESP_LOGI(TAG, "Commissioning session started");
    break;
  case DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
    ESP_LOGI(TAG, "Commissioning session stopped");
    break;
  case DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
    ESP_LOGI(TAG, "Commissioning window opened");
    break;
  case DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
    ESP_LOGI(TAG, "Commissioning window closed");
    break;
  case DeviceLayer::DeviceEventType::kFabricRemoved:
    ESP_LOGI(TAG, "Fabric removed successfully");
    break;
  case DeviceLayer::DeviceEventType::kFabricWillBeRemoved:
    ESP_LOGI(TAG, "Fabric will be removed");
    break;
  case DeviceLayer::DeviceEventType::kFabricUpdated:
    ESP_LOGI(TAG, "Fabric updated");
    break;
  case DeviceLayer::DeviceEventType::kFabricCommitted:
    ESP_LOGI(TAG, "Fabric committed");
    break;
  default:
    ESP_LOGD(TAG, "Device event: %u", (unsigned)event->Type);
    break;
  }
}

static esp_err_t on_identification(identification::callback_type_t type, uint16_t endpoint_id,
                                  uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
  return ESP_OK;
}

// The legacy endpoint is for connectivity tests; HVAC control is still local.
static esp_err_t on_attribute_update(attribute::callback_type_t type, uint16_t endpoint_id,
                                    uint32_t cluster_id, uint32_t attribute_id,
                                    esp_matter_attr_val_t *val, void *priv_data)
{
  return ESP_OK;
}

bool MatterInit()
{
  if (!OperatingParameters.MatterEnabled)
  {
    ESP_LOGI(TAG, "MatterInit called when Matter is not enabled");
    return false;
  }

  // Preserve the existing startup policy for provisioning regression tests.
  if (strlen(WifiCreds.ssid) == 0)
  {
    ESP_LOGI(TAG, "No wifi credentials set -- not starting Matter");
    return false;
  }

  if (!WifiProvClaimBleForMatter())
  {
    ESP_LOGW(TAG, "BLE was already claimed this boot; restart before starting Matter");
    return false;
  }

  WifiSetCredentials(WifiCreds.ssid, WifiCreds.password);
  esp_log_level_set("*", ESP_LOG_DEBUG);

  node::config_t node_config = {};
  strlcpy(node_config.root_node.basic_information.node_label, "RealSmartThermostat",
          sizeof(node_config.root_node.basic_information.node_label));
  node_t *node = node::create(&node_config, on_attribute_update, on_identification);
  if (node == nullptr)
  {
    ESP_LOGE(TAG, "Failed to create Matter node");
    return false;
  }

  endpoint::thermostat::config_t thermostat_config;
  thermostat_config.thermostat.local_temperature = 1900;
  thermostat_config.thermostat.system_mode = 0;
  thermostat_config.thermostat.feature_flags =
      cluster::thermostat::feature::heating::get_id() |
      cluster::thermostat::feature::cooling::get_id();
  thermostat_config.thermostat.features.heating.occupied_heating_setpoint = 2300;
  thermostat_config.thermostat.features.cooling.occupied_cooling_setpoint = 2600;
  endpoint_t *thermostat_endpoint = endpoint::thermostat::create(
      node, &thermostat_config, ENDPOINT_FLAG_NONE, nullptr);
  if (thermostat_endpoint == nullptr)
  {
    ESP_LOGE(TAG, "Failed to create thermostat endpoint");
    return false;
  }
  ESP_LOGI(TAG, "Thermostat endpoint_id = %u", endpoint::get_id(thermostat_endpoint));

  // These optional attributes moved out of the heating/cooling feature config
  // in the current SDK. Preserve the original limits and demand values.
  cluster_t *thermostat_cluster = cluster::get(thermostat_endpoint, Thermostat::Id);
  if (thermostat_cluster == nullptr)
  {
    ESP_LOGE(TAG, "Failed to get thermostat cluster");
    return false;
  }
  namespace thermostat_attributes = cluster::thermostat::attribute;
  attribute_t *optional_attributes[] = {
      thermostat_attributes::create_abs_min_heat_setpoint_limit(thermostat_cluster, 1500),
      thermostat_attributes::create_abs_max_heat_setpoint_limit(thermostat_cluster, 3000),
      thermostat_attributes::create_min_heat_setpoint_limit(thermostat_cluster, 2000),
      thermostat_attributes::create_max_heat_setpoint_limit(thermostat_cluster, 2800),
      thermostat_attributes::create_pi_heating_demand(thermostat_cluster, 0),
      thermostat_attributes::create_abs_min_cool_setpoint_limit(thermostat_cluster, 1600),
      thermostat_attributes::create_abs_max_cool_setpoint_limit(thermostat_cluster, 3200),
      thermostat_attributes::create_min_cool_setpoint_limit(thermostat_cluster, 1600),
      thermostat_attributes::create_max_cool_setpoint_limit(thermostat_cluster, 3200),
      thermostat_attributes::create_pi_cooling_demand(thermostat_cluster, 50),
  };
  for (attribute_t *optional_attribute : optional_attributes)
  {
    if (optional_attribute == nullptr)
    {
      ESP_LOGE(TAG, "Failed to create thermostat limit or demand attribute");
      return false;
    }
  }

  ESP_LOGI(TAG, "Heap before Matter: free_internal=%u largest_internal=%u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  esp_err_t err = esp_matter::start(on_device_event);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Matter start failed: %s", esp_err_to_name(err));
    return false;
  }

  MutableCharSpan qr(OperatingParameters.MatterQR, sizeof(OperatingParameters.MatterQR));
  MutableCharSpan manual(OperatingParameters.MatterPairingCode, sizeof(OperatingParameters.MatterPairingCode));
  CHIP_ERROR chip_err = GetQRCode(qr, RendezvousInformationFlag::kOnNetwork);
  if (chip_err == CHIP_NO_ERROR)
    ESP_LOGI(TAG, "QR Pairing Code: %s", OperatingParameters.MatterQR);
  else
    ESP_LOGE(TAG, "Failed to get QR code: %" CHIP_ERROR_FORMAT, chip_err.Format());
  chip_err = GetManualPairingCode(manual, RendezvousInformationFlag::kOnNetwork);
  if (chip_err == CHIP_NO_ERROR)
    ESP_LOGI(TAG, "Manual Pairing Code: %s", OperatingParameters.MatterPairingCode);
  else
    ESP_LOGE(TAG, "Failed to get manual pairing code: %" CHIP_ERROR_FORMAT, chip_err.Format());

  OperatingParameters.MatterStarted = true;
  WifiMatterStarted();
  ESP_LOGI(TAG, "Heap after Matter: free_internal=%u largest_internal=%u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return true;
}

void MatterFactoryReset()
{
  ESP_LOGI(TAG, "Calling Matter factory reset");
  esp_matter::factory_reset();
}

#endif
