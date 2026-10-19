// SPDX-License-Identifier: GPL-3.0-only

#include "thermostat.hpp"

#include <cstdio>
#include <cstring>
#include <cinttypes>
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "wifi_provisioning/scheme_softap.h"

static const char *TAG = "PROVISION";
static SemaphoreHandle_t s_lock;
static PROVISIONING_STATE s_state = PROVISIONING_IDLE;
static PROVISIONING_TRANSPORT s_transport = PROVISIONING_BLE;
static char s_status[80] = "Not running";
static char s_service_name[16];
static char s_pop[17];
static char s_qr_payload[192];
static bool s_manager_initialized;
static bool s_network_initialized;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static EventGroupHandle_t s_work_events;
static TaskHandle_t s_worker_task;

#define PROVISION_WORK_SAVE_CREDS BIT0
#define PROVISION_WORK_CONNECTED  BIT1
#define PROVISION_WORK_STOP       BIT2
#define PROVISION_WORK_STOPPED    BIT3

static void provisioning_worker(void *)
{
  bool done = false;
  while (!done) {
    EventBits_t work = xEventGroupWaitBits(s_work_events,
        PROVISION_WORK_SAVE_CREDS | PROVISION_WORK_CONNECTED | PROVISION_WORK_STOP,
        pdTRUE, pdFALSE, portMAX_DELAY);

    if (work & PROVISION_WORK_SAVE_CREDS) setWifiCreds();
    if ((work & PROVISION_WORK_CONNECTED) && !(work & PROVISION_WORK_STOP)) {
      // These operations can perform DNS, HTTPS, JSON parsing, and allocate
      // server resources. They must never run on ESP-IDF's sys_evt task.
      initTimeSntp();
      webStart();
      done = true;
    }
    if (work & PROVISION_WORK_STOP) done = true;
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_worker_task = nullptr;
  xSemaphoreGive(s_lock);
  xEventGroupSetBits(s_work_events, PROVISION_WORK_STOPPED);
  vTaskDelete(nullptr);
}

static bool start_worker()
{
  if (!s_lock || !s_work_events) return false;

  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool already_running = s_worker_task != nullptr;
  xSemaphoreGive(s_lock);
  if (already_running) return true;

  xEventGroupClearBits(s_work_events, PROVISION_WORK_SAVE_CREDS |
      PROVISION_WORK_CONNECTED | PROVISION_WORK_STOP | PROVISION_WORK_STOPPED);

  TaskHandle_t task = nullptr;
  if (xTaskCreate(provisioning_worker, "ProvisioningWorker", 12288, nullptr,
                  tskIDLE_PRIORITY + 1, &task) != pdPASS) {
    ESP_LOGE(TAG, "Could not allocate provisioning worker");
    return false;
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_worker_task = task;
  xSemaphoreGive(s_lock);
  return true;
}

static void stop_worker()
{
  if (!s_lock || !s_work_events) return;

  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool running = s_worker_task != nullptr;
  xSemaphoreGive(s_lock);
  if (!running) return;

  xEventGroupSetBits(s_work_events, PROVISION_WORK_STOP);
  xEventGroupWaitBits(s_work_events, PROVISION_WORK_STOPPED,
                      pdTRUE, pdTRUE, portMAX_DELAY);
}

static void set_state(PROVISIONING_STATE state, const char *status)
{
  if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
  s_state = state;
  strlcpy(s_status, status, sizeof(s_status));
  if (s_lock) xSemaphoreGive(s_lock);
}

static void load_identity()
{
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_service_name, sizeof(s_service_name), "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);

  nvs_handle_t handle;
  if (nvs_open("provision", NVS_READWRITE, &handle) != ESP_OK) return;
  size_t length = sizeof(s_pop);
  if (nvs_get_str(handle, "pop", s_pop, &length) != ESP_OK || strlen(s_pop) < 8) {
    snprintf(s_pop, sizeof(s_pop), "%08" PRIX32, esp_random());
    nvs_set_str(handle, "pop", s_pop);
    nvs_commit(handle);
  }
  nvs_close(handle);
}

static void provisioning_event(void *, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  if (event_base == WIFI_PROV_EVENT) {
    switch (event_id) {
      case WIFI_PROV_START:
        set_state(PROVISIONING_ADVERTISING, "Scan the QR code with ESP Provisioning");
        break;
      case WIFI_PROV_CRED_RECV: {
        const wifi_sta_config_t *credentials = static_cast<const wifi_sta_config_t *>(event_data);
        strlcpy(WifiCreds.ssid, reinterpret_cast<const char *>(credentials->ssid), sizeof(WifiCreds.ssid));
        strlcpy(WifiCreds.password, reinterpret_cast<const char *>(credentials->password), sizeof(WifiCreds.password));
        xEventGroupSetBits(s_work_events, PROVISION_WORK_SAVE_CREDS);
        set_state(PROVISIONING_CONNECTING, "Connecting to Wi-Fi...");
        break;
      }
      case WIFI_PROV_CRED_FAIL: {
        const wifi_prov_sta_fail_reason_t reason = *static_cast<wifi_prov_sta_fail_reason_t *>(event_data);
        set_state(PROVISIONING_FAILED, reason == WIFI_PROV_STA_AUTH_ERROR
          ? "Wi-Fi authentication failed; retry in the app"
          : "Access point not found; retry in the app");
        break;
      }
      case WIFI_PROV_CRED_SUCCESS:
        WifiStatus.Connected = true;
        OperatingParameters.wifiConnected = true;
        set_state(PROVISIONING_SUCCEEDED, "Provisioning complete");
        break;
      case WIFI_PROV_END:
        if (s_manager_initialized) {
          s_manager_initialized = false;
          wifi_prov_mgr_deinit();
        }
        if (s_ap_netif) {
          esp_netif_destroy_default_wifi(s_ap_netif);
          s_ap_netif = nullptr;
        }
        if (s_state == PROVISIONING_SUCCEEDED)
          xEventGroupSetBits(s_work_events, PROVISION_WORK_CONNECTED);
        break;
      default:
        break;
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP && s_manager_initialized) {
    const ip_event_got_ip_t *event = static_cast<const ip_event_got_ip_t *>(event_data);
    WifiStatus.ip = event->ip_info.ip;
    WifiStatus.Connected = true;
    WifiStatus.wifi_started = true;
    OperatingParameters.wifiConnected = true;
  }
}

void ProvisioningInit()
{
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  if (!s_work_events) s_work_events = xEventGroupCreate();
  if (!s_lock || !s_work_events)
    ESP_LOGE(TAG, "Could not allocate provisioning synchronization objects");
  load_identity();
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, provisioning_event, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, provisioning_event, nullptr));
}

bool ProvisioningStart(PROVISIONING_TRANSPORT transport)
{
  ProvisioningStop();
  // SoftAP provisioning owns the HTTP server port; stopping is harmless for BLE too.
  webStop();
  set_state(PROVISIONING_STARTING, "Starting provisioning...");
  s_transport = transport;

  if (!start_worker()) {
    set_state(PROVISIONING_FAILED, "Not enough memory for provisioning");
    return false;
  }

  if (!s_network_initialized) {
    // A failed/timeout connection stops the radio but leaves the Wi-Fi driver
    // and default STA netif allocated. Tear down based on ownership state,
    // not WifiStarted(), or creating the provisioning STA netif will assert.
    if (WifiStatus.driver_started || WifiStatus.if_init) WifiDeinit();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (transport == PROVISIONING_SOFTAP) s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t wifi_err = esp_wifi_init(&wifi_config);
    if (!s_sta_netif || (transport == PROVISIONING_SOFTAP && !s_ap_netif) || wifi_err != ESP_OK) {
      ESP_LOGE(TAG, "Wi-Fi initialization failed: %s", esp_err_to_name(wifi_err));
      stop_worker();
      set_state(PROVISIONING_FAILED, "Could not initialize Wi-Fi");
      return false;
    }
    s_network_initialized = true;
    WifiAdoptNetwork(s_sta_netif);
  } else if (transport == PROVISIONING_SOFTAP && !s_ap_netif) {
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
      stop_worker();
      set_state(PROVISIONING_FAILED, "Could not initialize SoftAP");
      return false;
    }
  }

  wifi_prov_mgr_config_t config = {};
  config.scheme = transport == PROVISIONING_BLE ? wifi_prov_scheme_ble : wifi_prov_scheme_softap;
  // Keep BT memory available so BLE provisioning can be started again from the touchscreen.
  config.scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE;
  config.app_event_handler = WIFI_PROV_EVENT_HANDLER_NONE;

  esp_err_t err = wifi_prov_mgr_init(config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Manager initialization failed: %s", esp_err_to_name(err));
    stop_worker();
    set_state(PROVISIONING_FAILED, "Could not initialize provisioning");
    return false;
  }
  s_manager_initialized = true;

  const char *transport_name = transport == PROVISIONING_BLE ? "ble" : "softap";
  snprintf(s_qr_payload, sizeof(s_qr_payload),
           "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"%s\"}",
           s_service_name, s_pop, transport_name);

  err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, s_pop, s_service_name,
                                         transport == PROVISIONING_SOFTAP ? s_pop : nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Provisioning start failed: %s", esp_err_to_name(err));
    wifi_prov_mgr_deinit();
    s_manager_initialized = false;
    stop_worker();
    set_state(PROVISIONING_FAILED, "Could not start provisioning");
    return false;
  }
  ESP_LOGI(TAG, "Provisioning started as %s over %s", s_service_name, transport_name);
  return true;
}

void ProvisioningStop()
{
  if (s_manager_initialized) {
    s_manager_initialized = false;
    wifi_prov_mgr_stop_provisioning();
    wifi_prov_mgr_deinit();
  }
  if (s_ap_netif) {
    esp_netif_destroy_default_wifi(s_ap_netif);
    s_ap_netif = nullptr;
  }
  stop_worker();
  set_state(PROVISIONING_IDLE, "Not running");
  if (WifiConnected()) webStart();
}

void ProvisioningNotifyWifiDeinit()
{
  s_network_initialized = false;
  s_sta_netif = nullptr;
  s_ap_netif = nullptr;
}

bool ProvisioningActive()
{
  return s_state == PROVISIONING_STARTING || s_state == PROVISIONING_ADVERTISING ||
         s_state == PROVISIONING_RECEIVING || s_state == PROVISIONING_CONNECTING ||
         s_state == PROVISIONING_FAILED;
}

PROVISIONING_STATE ProvisioningGetState() { return s_state; }
const char *ProvisioningGetStatus() { return s_status; }
const char *ProvisioningGetServiceName() { return s_service_name; }
const char *ProvisioningGetQrPayload() { return s_qr_payload; }
PROVISIONING_TRANSPORT ProvisioningGetTransport() { return s_transport; }
