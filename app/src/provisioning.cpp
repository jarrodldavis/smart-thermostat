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
static bool s_initialized;
static bool s_manager_initialized;
static bool s_session_end_pending;
static bool s_session_end_cleanup;
static bool s_network_initialized;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static EventGroupHandle_t s_work_events;
static TaskHandle_t s_worker_task;

#define PROVISION_WORK_SAVE_CREDS BIT0
#define PROVISION_WORK_CONNECTED  BIT1
#define PROVISION_WORK_STOP       BIT2
#define PROVISION_WORK_STOPPED    BIT3
#define PROVISION_SESSION_ENDED   BIT4

static void mark_disconnected(bool reconnect)
{
  WifiStatus.Connected = false;
  WifiStatus.ip.addr = 0;
  WifiStatus.reconnect_requested = reconnect && strlen(WifiCreds.ssid) != 0;
  OperatingParameters.wifiConnected = false;
}

static void restore_normal_network(bool reconnect)
{
  WifiRegisterEventCallbacks();
  mark_disconnected(reconnect);
}

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

  // Publish completion and relinquish ownership while holding the same lock
  // used by start/stop. A replacement worker cannot clear the completion bit
  // between these two operations.
  xSemaphoreTake(s_lock, portMAX_DELAY);
  xEventGroupSetBits(s_work_events, PROVISION_WORK_STOPPED);
  s_worker_task = nullptr;
  xSemaphoreGive(s_lock);
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

static bool load_identity()
{
  uint8_t mac[6];
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    ESP_LOGE(TAG, "Could not read device identity");
    return false;
  }
  snprintf(s_service_name, sizeof(s_service_name), "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);

  nvs_handle_t handle;
  esp_err_t err = nvs_open("provision", NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not open provisioning identity storage: %s", esp_err_to_name(err));
    return false;
  }

  char pop[sizeof(s_pop)] = {};
  size_t length = sizeof(pop);
  err = nvs_get_str(handle, "pop", pop, &length);
  if (err != ESP_OK || strlen(pop) < 8) {
    snprintf(pop, sizeof(pop), "%08" PRIX32, esp_random());
    err = nvs_set_str(handle, "pop", pop);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Could not persist provisioning identity: %s", esp_err_to_name(err));
      nvs_close(handle);
      return false;
    }
  }

  strlcpy(s_pop, pop, sizeof(s_pop));
  nvs_close(handle);
  return true;
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
      case WIFI_PROV_END: {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_session_end_pending || s_session_end_cleanup) {
          xSemaphoreGive(s_lock);
          break;
        }
        s_session_end_cleanup = true;
        const bool manager_initialized = s_manager_initialized;
        s_manager_initialized = false;
        xSemaphoreGive(s_lock);

        if (manager_initialized) {
          wifi_prov_mgr_deinit();
        }
        if (s_ap_netif) {
          esp_netif_destroy_default_wifi(s_ap_netif);
          s_ap_netif = nullptr;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool succeeded = s_state == PROVISIONING_SUCCEEDED;
        xSemaphoreGive(s_lock);
        if (succeeded) {
          // Provisioning owns Wi-Fi events while its manager is active. Resume
          // the thermostat handlers so future disconnects update state and
          // trigger the normal reconnect path.
          WifiRegisterEventCallbacks();
          xEventGroupSetBits(s_work_events, PROVISION_WORK_CONNECTED);
        }
        // wifi_prov_mgr_deinit() posts this event to the default event loop.
        // Publish completion only after all old-session cleanup has run so a
        // transport switch cannot expose a new manager to this callback.
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_session_end_pending = false;
        s_session_end_cleanup = false;
        xEventGroupSetBits(s_work_events, PROVISION_SESSION_ENDED);
        xSemaphoreGive(s_lock);
        break;
      }
      default:
        break;
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool manager_initialized = s_manager_initialized;
    xSemaphoreGive(s_lock);
    if (!manager_initialized) return;

    const ip_event_got_ip_t *event = static_cast<const ip_event_got_ip_t *>(event_data);
    WifiStatus.ip = event->ip_info.ip;
    WifiStatus.Connected = true;
    WifiStatus.wifi_started = true;
    WifiStatus.reconnect_requested = false;
    OperatingParameters.wifiConnected = true;
  }
}

void ProvisioningInit()
{
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  if (!s_work_events) s_work_events = xEventGroupCreate();
  if (!s_lock || !s_work_events) {
    ESP_LOGE(TAG, "Could not allocate provisioning synchronization objects");
    set_state(PROVISIONING_FAILED, "Not enough memory for provisioning");
    return;
  }
  if (!load_identity()) {
    set_state(PROVISIONING_FAILED, "Could not initialize device identity");
    return;
  }
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, provisioning_event, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, provisioning_event, nullptr));
  s_initialized = true;
}

static bool stop_session(bool resume_network)
{
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool manager_initialized = s_manager_initialized;
  const bool end_pending = s_session_end_pending;
  s_manager_initialized = false;
  xSemaphoreGive(s_lock);
  const bool was_active = manager_initialized || ProvisioningActive();

  if (manager_initialized) {
    // The END event is queued asynchronously. Wait for its handler before
    // returning so ProvisioningStart() can safely initialize another scheme.
    wifi_prov_mgr_stop_provisioning();
    wifi_prov_mgr_deinit();
  }
  if (end_pending) {
    xEventGroupWaitBits(s_work_events, PROVISION_SESSION_ENDED,
                        pdTRUE, pdTRUE, portMAX_DELAY);
  }
  if (s_ap_netif) {
    esp_netif_destroy_default_wifi(s_ap_netif);
    s_ap_netif = nullptr;
  }
  stop_worker();
  set_state(PROVISIONING_IDLE, "Not running");

  if (resume_network && was_active) {
    restore_normal_network(true);
    // The HTTP server binds independently of the STA address and will be
    // reachable once the normal reconnect task obtains an address.
    webStart();
  }
  return was_active;
}

bool ProvisioningStart(PROVISIONING_TRANSPORT transport)
{
  if (!s_initialized) {
    set_state(PROVISIONING_FAILED, "Provisioning is unavailable");
    return false;
  }

  stop_session(false);
  // SoftAP provisioning owns the HTTP server port; stopping is harmless for BLE too.
  webStop();
  // Provisioning owns Wi-Fi/IP events for the duration of the session. This
  // is also required for repeated provisioning after a successful session.
  WifiDeregisterEventCallbacks();
  mark_disconnected(false);
  set_state(PROVISIONING_STARTING, "Starting provisioning...");
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_transport = transport;
  xSemaphoreGive(s_lock);

  if (!start_worker()) {
    restore_normal_network(true);
    webStart();
    set_state(PROVISIONING_FAILED, "Not enough memory for provisioning");
    return false;
  }

  if (!s_network_initialized) {
    // A failed/timeout connection stops the radio but leaves the Wi-Fi driver
    // and default STA netif allocated. Tear down based on ownership state,
    // not WifiStarted(), or creating the provisioning STA netif will assert.
    if (WifiStatus.driver_started || WifiStatus.if_init) WifiDeinit();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
      ESP_LOGE(TAG, "Could not create provisioning STA interface");
      stop_worker();
      restore_normal_network(true);
      webStart();
      set_state(PROVISIONING_FAILED, "Could not initialize Wi-Fi");
      return false;
    }
    if (transport == PROVISIONING_SOFTAP) {
      s_ap_netif = esp_netif_create_default_wifi_ap();
      if (!s_ap_netif) {
        ESP_LOGE(TAG, "Could not create provisioning SoftAP interface");
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = nullptr;
        stop_worker();
        restore_normal_network(true);
        webStart();
        set_state(PROVISIONING_FAILED, "Could not initialize SoftAP");
        return false;
      }
    }
    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t wifi_err = esp_wifi_init(&wifi_config);
    if (wifi_err != ESP_OK) {
      ESP_LOGE(TAG, "Wi-Fi initialization failed: %s", esp_err_to_name(wifi_err));
      if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = nullptr;
      }
      esp_netif_destroy_default_wifi(s_sta_netif);
      s_sta_netif = nullptr;
      stop_worker();
      restore_normal_network(true);
      webStart();
      set_state(PROVISIONING_FAILED, "Could not initialize Wi-Fi");
      return false;
    }
    s_network_initialized = true;
    WifiAdoptNetwork(s_sta_netif);
  } else if (transport == PROVISIONING_SOFTAP && !s_ap_netif) {
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
      stop_worker();
      restore_normal_network(true);
      webStart();
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
    restore_normal_network(true);
    webStart();
    set_state(PROVISIONING_FAILED, "Could not initialize provisioning");
    return false;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_manager_initialized = true;
  s_session_end_pending = true;
  s_session_end_cleanup = false;
  xEventGroupClearBits(s_work_events, PROVISION_SESSION_ENDED);
  xSemaphoreGive(s_lock);

  const char *transport_name = transport == PROVISIONING_BLE ? "ble" : "softap";
  xSemaphoreTake(s_lock, portMAX_DELAY);
  snprintf(s_qr_payload, sizeof(s_qr_payload),
           "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"%s\"}",
           s_service_name, s_pop, transport_name);
  xSemaphoreGive(s_lock);

  err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, s_pop, s_service_name,
                                         transport == PROVISIONING_SOFTAP ? s_pop : nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Provisioning start failed: %s", esp_err_to_name(err));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool cleanup_in_progress = s_session_end_cleanup;
    const bool manager_initialized = s_manager_initialized;
    if (!cleanup_in_progress) {
      s_session_end_cleanup = true;
      s_manager_initialized = false;
    }
    xSemaphoreGive(s_lock);

    if (cleanup_in_progress) {
      xEventGroupWaitBits(s_work_events, PROVISION_SESSION_ENDED,
                          pdTRUE, pdTRUE, portMAX_DELAY);
    } else {
      if (manager_initialized) wifi_prov_mgr_deinit();
      if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = nullptr;
      }
      xSemaphoreTake(s_lock, portMAX_DELAY);
      // A failed start is not guaranteed to emit WIFI_PROV_END. Complete the
      // session here and ignore any late END event.
      s_session_end_pending = false;
      xEventGroupSetBits(s_work_events, PROVISION_SESSION_ENDED);
      s_session_end_cleanup = false;
      xSemaphoreGive(s_lock);
    }
    stop_worker();
    restore_normal_network(true);
    webStart();
    set_state(PROVISIONING_FAILED, "Could not start provisioning");
    return false;
  }
  ESP_LOGI(TAG, "Provisioning started as %s over %s", s_service_name, transport_name);
  return true;
}

void ProvisioningStop()
{
  if (!s_initialized) {
    set_state(PROVISIONING_IDLE, "Not running");
    return;
  }
  stop_session(true);
}

void ProvisioningNotifyWifiDeinit()
{
  s_network_initialized = false;
  s_sta_netif = nullptr;
  s_ap_netif = nullptr;
}

bool ProvisioningActive()
{
  if (!s_lock) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool active = s_state == PROVISIONING_STARTING || s_state == PROVISIONING_ADVERTISING ||
                      s_state == PROVISIONING_RECEIVING || s_state == PROVISIONING_CONNECTING ||
                      (s_state == PROVISIONING_FAILED && s_manager_initialized);
  xSemaphoreGive(s_lock);
  return active;
}

bool ProvisioningGetSnapshot(PROVISIONING_SNAPSHOT *snapshot)
{
  if (!snapshot || !s_lock) return false;

  xSemaphoreTake(s_lock, portMAX_DELAY);
  snapshot->state = s_state;
  snapshot->transport = s_transport;
  strlcpy(snapshot->status, s_status, sizeof(snapshot->status));
  strlcpy(snapshot->service_name, s_service_name, sizeof(snapshot->service_name));
  strlcpy(snapshot->qr_payload, s_qr_payload, sizeof(snapshot->qr_payload));
  xSemaphoreGive(s_lock);
  return true;
}
