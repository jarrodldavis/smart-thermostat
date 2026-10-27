// SPDX-License-Identifier: GPL-3.0-only
#ifdef MATTER_ENABLED

#include "thermostat.hpp"
#include <esp_matter.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/internal/BLEManager.h>
#include <app/server/Server.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <sys/time.h>

using namespace chip;
using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "Matter";
static portMUX_TYPE pairingMux = portMUX_INITIALIZER_UNLOCKED;
static MATTER_PAIRING_STATE pairingState = {};
static MATTER_PAIRING_TRANSPORT pairingTransport = MATTER_PAIRING_TRANSPORT::NETWORK;
static bool pairingBleWindowStarted = false;
static bool pairingBleShutdownRequested = false;
static bool pairingBleMemoryReleased = !CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE;
static bool pairingSessionActive = false;
static bool networkSetupStarted = false;
static bool matterCommissioned = false;

// Run TLS timezone discovery only after BLE has returned its memory. Never
// block the CHIP event task with HTTP, NVS writes or time synchronization.
static void finishMatterNetworkSetup()
{
  if (networkSetupStarted || !pairingBleMemoryReleased || !WifiConnected() || !matterCommissioned)
    return;
  networkSetupStarted = true;
  if (xTaskCreate([](void *) {
        wifi_config_t config = {};
        if (esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK && config.sta.ssid[0] != 0)
        {
          // Driver fields can fill their entire fixed-width buffers.
          char ssid[sizeof(config.sta.ssid) + 1] = {};
          char password[sizeof(config.sta.password) + 1] = {};
          memcpy(ssid, config.sta.ssid, sizeof(config.sta.ssid));
          memcpy(password, config.sta.password, sizeof(config.sta.password));
          if (strcmp(WifiCreds.ssid, ssid) != 0 || strcmp(WifiCreds.password, password) != 0)
          {
            strlcpy(WifiCreds.ssid, ssid, sizeof(WifiCreds.ssid));
            strlcpy(WifiCreds.password, password, sizeof(WifiCreds.password));
            setWifiCreds();
            ESP_LOGI(TAG, "Saved Wi-Fi credentials accepted by Matter commissioning");
          }
          memset(password, 0, sizeof(password));
        }
        memset(&config, 0, sizeof(config));
        initTimeSntp();
        ESP_LOGI(TAG, "Matter network setup complete");
        vTaskDelete(nullptr);
      }, "matter_network", 8192, nullptr, 1, nullptr) != pdPASS)
  {
    networkSetupStarted = false;
    ESP_LOGE(TAG, "Could not start Matter network setup task");
  }
}

// Runs on the CHIP thread. LVGL reads only the small, locked snapshot below.
static void publishPairingState(const char *status = nullptr)
{
  MATTER_PAIRING_STATE next = {};
  next.started = OperatingParameters.MatterStarted;
  next.paired = Server::GetInstance().GetFabricTable().FabricCount() != 0;
  next.windowOpen = Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();
  next.sessionActive = pairingSessionActive;
  next.wifiConnected = WifiConnected();
  next.bleSupported = CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE;
  next.bleRestartRequired = next.bleSupported &&
      (pairingBleShutdownRequested || pairingBleMemoryReleased || WifiProvBleUsedThisBoot());
  next.transport = pairingTransport;
  next.showCode = next.windowOpen && !next.paired && status == nullptr;
  if (status == nullptr)
  {
    if (next.paired)
      status = "Already paired. Use your controller to share this device.";
    else if (next.windowOpen)
      status = pairingTransport == MATTER_PAIRING_TRANSPORT::BLE ? "BLE pairing open" : "Network pairing open";
    else if (next.bleRestartRequired)
      status = next.wifiConnected ? "Choose Network. Restart the device to use BLE again."
                                  : "Restart the device to set up Wi-Fi and Matter over BLE.";
    else if (next.bleSupported)
      status = "Choose Network or BLE to pair with a Matter controller.";
    else
      status = "Choose Network to pair with a Matter controller.";
  }
  strlcpy(next.status, status, sizeof(next.status));
  strlcpy(next.qr, OperatingParameters.MatterQR, sizeof(next.qr));
  strlcpy(next.manual, OperatingParameters.MatterPairingCode, sizeof(next.manual));
  portENTER_CRITICAL(&pairingMux);
  pairingState = next;
  portEXIT_CRITICAL(&pairingMux);
}

// Use the same shutdown interface as esp-matter's post-commissioning cleanup.
// It stops NimBLE and, with USE_BLE_ONLY_FOR_COMMISSIONING, permanently returns
// Bluetooth memory to the heap. Run only on the CHIP thread.
static void releasePairingBle()
{
#if CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE
  if (pairingBleShutdownRequested || pairingBleMemoryReleased)
    return;
  pairingBleShutdownRequested = true;
  ESP_LOGI(TAG, "Releasing BLE: free_internal=%u largest_internal=%u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  DeviceLayer::Internal::BLEMgr().Shutdown();
#endif
}

static void releaseStoppedPairingBle()
{
  // Closing the advertising window during PASE is normal. Keep the transport
  // alive until that session completes or fails; Stop/timeout with no session
  // can release it immediately.
  if (pairingBleWindowStarted && !pairingSessionActive &&
      !Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen())
    releasePairingBle();
}

static void on_device_event(const ChipDeviceEvent *event, intptr_t arg)
{
  switch (event->Type)
  {
  case DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
    ESP_LOGI(TAG, "Interface IP address changed");
    WifiMatterStarted();
    finishMatterNetworkSetup();
    publishPairingState();
    break;
  case DeviceLayer::DeviceEventType::kWiFiConnectivityChange:
    ESP_LOGI(TAG, "Wi-Fi connectivity changed");
    WifiMatterStarted();
    finishMatterNetworkSetup();
    publishPairingState();
    break;
  case DeviceLayer::DeviceEventType::kCommissioningComplete:
    ESP_LOGI(TAG, "Commissioning complete");
    matterCommissioned = true;
    pairingSessionActive = false;
    finishMatterNetworkSetup();
    releaseStoppedPairingBle();
    publishPairingState();
    break;
  case DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
    ESP_LOGI(TAG, "Commissioning failed, fail-safe timer expired");
    pairingSessionActive = false;
    releaseStoppedPairingBle();
    publishPairingState();
    break;
  case DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
    ESP_LOGI(TAG, "Commissioning session started");
    pairingSessionActive = true;
    publishPairingState();
    break;
  case DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
    ESP_LOGI(TAG, "Commissioning session stopped");
    pairingSessionActive = false;
    releaseStoppedPairingBle();
    publishPairingState();
    break;
  case DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
    ESP_LOGI(TAG, "Commissioning window opened");
    publishPairingState();
    break;
  case DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
    ESP_LOGI(TAG, "Commissioning window closed");
    releaseStoppedPairingBle();
    publishPairingState();
    break;
  case DeviceLayer::DeviceEventType::kBLEDeinitialized:
    pairingBleMemoryReleased = true;
    pairingBleShutdownRequested = true;
    finishMatterNetworkSetup();
    ESP_LOGI(TAG, "BLE memory released: free_internal=%u largest_internal=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    publishPairingState();
    break;
  case DeviceLayer::DeviceEventType::kFabricRemoved:
    ESP_LOGI(TAG, "Fabric removed successfully");
    matterCommissioned = Server::GetInstance().GetFabricTable().FabricCount() != 0;
    publishPairingState();
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

#if CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE
  // This SDK initializes BLE at startup when CHIP-over-BLE is enabled.
  if (!WifiProvClaimBleForMatter())
  {
    ESP_LOGW(TAG, "BLE was already claimed this boot; restart before starting Matter");
    return false;
  }
#else
  // Use standalone Wi-Fi provisioning first when BLE commissioning is omitted.
  if (strlen(WifiCreds.ssid) == 0)
  {
    ESP_LOGI(TAG, "Waiting for Wi-Fi provisioning before starting Matter");
    return false;
  }
#endif

  // A successful provisioning session already configured and connected this
  // driver. Reuse that connection rather than rewriting its configuration.
  if (strlen(WifiCreds.ssid) != 0 && !WifiConnected())
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
  endpoint_t *thermostat_endpoint = endpoint::create(node, ENDPOINT_FLAG_NONE, nullptr);
  if (thermostat_endpoint == nullptr)
  {
    ESP_LOGE(TAG, "Failed to create thermostat endpoint");
    return false;
  }
  // Predefine LocalTemperature using the public data-model API. This SDK's
  // thermostat helper marks it internally managed, but supplies no internal
  // reader for the exposed-temperature feature, so controller reads fail.
  // The helper retains attributes already supplied by the application.
  cluster_t *thermostat_cluster = cluster::create(thermostat_endpoint, Thermostat::Id, CLUSTER_FLAG_SERVER);
  if (cluster::descriptor::create(thermostat_endpoint, &thermostat_config.descriptor, CLUSTER_FLAG_SERVER) == nullptr ||
      thermostat_cluster == nullptr ||
      attribute::create(thermostat_cluster, Thermostat::Attributes::LocalTemperature::Id,
                        ATTRIBUTE_FLAG_NULLABLE,
                        esp_matter_nullable_int16(thermostat_config.thermostat.local_temperature)) == nullptr ||
      endpoint::thermostat::add(thermostat_endpoint, &thermostat_config) != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to configure thermostat endpoint");
    return false;
  }
  ESP_LOGI(TAG, "Thermostat endpoint_id = %u", endpoint::get_id(thermostat_endpoint));

  // These optional attributes moved out of the heating/cooling feature config
  // in the current SDK. Preserve the original limits and demand values.
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
  // This SDK initializes its clock to 2000 even if SNTP has already synced.
  // Preserve valid wall time across startup without another network request.
  constexpr time_t validClockThreshold = 1451606400; // 2016-01-01, as in getLocalTime().
  struct timeval synchronizedTime = {};
  gettimeofday(&synchronizedTime, nullptr);
  const int64_t synchronizedAt = esp_timer_get_time();
  esp_err_t err = esp_matter::start(on_device_event);
  if (synchronizedTime.tv_sec >= validClockThreshold && time(nullptr) < validClockThreshold)
  {
    const int64_t elapsedUs = esp_timer_get_time() - synchronizedAt;
    const int64_t restoredUs = int64_t(synchronizedTime.tv_sec) * 1000000 + synchronizedTime.tv_usec + elapsedUs;
    synchronizedTime.tv_sec = restoredUs / 1000000;
    synchronizedTime.tv_usec = restoredUs % 1000000;
    if (settimeofday(&synchronizedTime, nullptr) != 0)
      ESP_LOGE(TAG, "Failed to restore synchronized clock after Matter initialization");
    else
      ESP_LOGI(TAG, "Preserved synchronized clock across Matter initialization");
  }
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
  chip_err = DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
    matterCommissioned = Server::GetInstance().GetFabricTable().FabricCount() != 0;
    finishMatterNetworkSetup();
    publishPairingState();
  });
  if (chip_err != CHIP_NO_ERROR)
    ESP_LOGE(TAG, "Failed to publish pairing state: %s", chip_err.AsString());
  ESP_LOGI(TAG, "Heap after Matter: free_internal=%u largest_internal=%u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return true;
}

void MatterGetPairingState(MATTER_PAIRING_STATE *state)
{
  portENTER_CRITICAL(&pairingMux);
  *state = pairingState;
  portEXIT_CRITICAL(&pairingMux);
}

bool MatterPairingActive()
{
  portENTER_CRITICAL(&pairingMux);
  bool active = pairingState.windowOpen || pairingState.sessionActive;
  portEXIT_CRITICAL(&pairingMux);
  return active;
}

void MatterOpenCommissioningWindow(MATTER_PAIRING_TRANSPORT transport)
{
  if (!OperatingParameters.MatterStarted)
    return;

  CHIP_ERROR err = DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t requested) {
    const auto transport = static_cast<MATTER_PAIRING_TRANSPORT>(requested);
    auto &server = Server::GetInstance();
    auto &window = server.GetCommissioningWindowManager();
    if (server.GetFabricTable().FabricCount() != 0)
    {
      publishPairingState();
      return;
    }
    if (transport == MATTER_PAIRING_TRANSPORT::NETWORK && !WifiConnected())
    {
      publishPairingState("Connect Wi-Fi first, or choose BLE to set up Wi-Fi and Matter together.");
      return;
    }
    if (WifiProvActive())
    {
      publishPairingState("Finish Wi-Fi provisioning first.");
      return;
    }
    if (window.IsCommissioningWindowOpen() && transport == pairingTransport)
    {
      publishPairingState();
      return;
    }
    if (pairingSessionActive)
    {
      publishPairingState("Pairing in progress. Wait for it to finish or time out.");
      return;
    }
    if (transport == MATTER_PAIRING_TRANSPORT::BLE &&
        (!CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE || pairingBleShutdownRequested || pairingBleMemoryReleased || WifiProvBleUsedThisBoot()))
    {
      publishPairingState(WifiConnected() ? "Restart the device to use BLE. Network pairing remains available."
                                          : "Restart the device to set up Wi-Fi and Matter over BLE.");
      return;
    }
    if (window.IsCommissioningWindowOpen())
      window.CloseCommissioningWindow();

    RendezvousInformationFlags rendezvous;
    rendezvous.Set(transport == MATTER_PAIRING_TRANSPORT::BLE ? RendezvousInformationFlag::kBLE : RendezvousInformationFlag::kOnNetwork);
    MutableCharSpan qr(OperatingParameters.MatterQR, sizeof(OperatingParameters.MatterQR));
    MutableCharSpan manual(OperatingParameters.MatterPairingCode, sizeof(OperatingParameters.MatterPairingCode));
    CHIP_ERROR codeErr = GetQRCode(qr, rendezvous);
    if (codeErr == CHIP_NO_ERROR)
      codeErr = GetManualPairingCode(manual, rendezvous);
    if (codeErr != CHIP_NO_ERROR)
    {
      publishPairingState("Could not generate pairing code.");
      ESP_LOGE(TAG, "Pairing code failed: %s", codeErr.AsString());
      return;
    }
    pairingTransport = transport;
    if (transport == MATTER_PAIRING_TRANSPORT::BLE)
      pairingBleWindowStarted = true;
    else
      releasePairingBle();
    CHIP_ERROR openErr = window.OpenBasicCommissioningWindow(System::Clock::Seconds32(300),
        transport == MATTER_PAIRING_TRANSPORT::BLE ? CommissioningWindowAdvertisement::kAllSupported
                                                 : CommissioningWindowAdvertisement::kDnssdOnly);
    if (openErr != CHIP_NO_ERROR)
    {
      publishPairingState("Could not start pairing. Try Network or restart for BLE.");
      ESP_LOGE(TAG, "Failed to open commissioning window: %s", openErr.AsString());
      releaseStoppedPairingBle();
    }
    else
    {
      ESP_LOGI(TAG, "Opened %s commissioning window", transport == MATTER_PAIRING_TRANSPORT::BLE ? "BLE" : "network");
      publishPairingState();
    }
  }, static_cast<intptr_t>(transport));
  if (err != CHIP_NO_ERROR)
    ESP_LOGE(TAG, "Failed to schedule commissioning window: %s", err.AsString());
}

void MatterCloseCommissioningWindow()
{
  if (!OperatingParameters.MatterStarted)
    return;
  CHIP_ERROR err = DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
    if (Server::GetInstance().GetFabricTable().FabricCount() == 0)
      Server::GetInstance().GetCommissioningWindowManager().CloseCommissioningWindow();
    releaseStoppedPairingBle();
    publishPairingState();
  });
  if (err != CHIP_NO_ERROR)
    ESP_LOGE(TAG, "Failed to stop pairing: %s", err.AsString());
}

void MatterFactoryReset()
{
  ESP_LOGI(TAG, "Calling Matter factory reset");
  esp_matter::factory_reset();
}

#endif
