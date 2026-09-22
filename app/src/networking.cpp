#include "thermostat.hpp"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <algorithm>

static const char *TAG = "NETWORK";

static constexpr int64_t RECONNECT_FAST_DELAY_US = 10 * 1000000LL;
static constexpr int64_t RECONNECT_SLOW_DELAY_US = 30 * 1000000LL;
static constexpr int64_t RECONNECT_FAST_PERIOD_US = 60 * 1000000LL;

// Resource lifetime: initialized once by the startup task.
static bool netif_initialized = false;
static bool network_initialized = false;
static esp_netif_t *station_interface = nullptr;

ESP_EVENT_DEFINE_BASE(NETWORK_EVENT);

enum {
  NETWORK_EVENT_TICK,
  NETWORK_EVENT_CONNECT_SAVED,
};

static esp_timer_handle_t network_timer = nullptr;

// Connection policy: accessed only by the default event-loop task.
static bool normal_connection_enabled = false;
static int64_t reconnect_due_us = 0;
static int64_t reconnect_started_us = -1;

// Shared snapshot: every runtime access holds status_lock.
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static const esp_ip4_addr_t NO_IP = {.addr = 0};
static const uint8_t NO_DISCONNECT_REASON = 0;
static NETWORK_STATUS current_status = {
  .link = NETWORK_OFFLINE,
  .ip = NO_IP,
  .disconnect_reason = NO_DISCONNECT_REASON,
  .ssid = "",
  .rssi = 0,
  .rssi_valid = false
};

// Status transitions own their locks. No driver calls or logging under the lock.
static void clearConnectionStatus(NETWORK_LINK_STATE link, uint8_t reason = 0)
{
  portENTER_CRITICAL(&status_lock);
  current_status = {};
  current_status.link = link;
  current_status.disconnect_reason = reason;
  portEXIT_CRITICAL(&status_lock);
}

static void setAssociatedStatus(const uint8_t *ssid, size_t ssid_len)
{
  portENTER_CRITICAL(&status_lock);
  current_status.link = NETWORK_WAITING_FOR_IP;
  current_status.ip = NO_IP;
  memcpy(current_status.ssid, ssid, ssid_len);
  current_status.ssid[ssid_len] = '\0';
  current_status.rssi = 0;
  current_status.rssi_valid = false;
  portEXIT_CRITICAL(&status_lock);
}

static void setReadyStatus(esp_ip4_addr_t ip)
{
  portENTER_CRITICAL(&status_lock);
  current_status.link = NETWORK_READY;
  current_status.ip = ip;
  current_status.disconnect_reason = NO_DISCONNECT_REASON;
  portEXIT_CRITICAL(&status_lock);
}

static void clearIpStatus()
{
  portENTER_CRITICAL(&status_lock);
  current_status.ip = NO_IP;
  if (current_status.link == NETWORK_READY) {
    current_status.link = NETWORK_WAITING_FOR_IP;
  }
  portEXIT_CRITICAL(&status_lock);
}

// Connection and retry policy (default event-loop task only).
static void scheduleReconnect()
{
  if (!normal_connection_enabled || reconnect_started_us >= 0) {
    return;
  }

  reconnect_started_us = esp_timer_get_time();
  reconnect_due_us = reconnect_started_us; // Eligible on the next timer tick.
}

static void resetReconnect()
{
  reconnect_started_us = -1;
  reconnect_due_us = 0;
}

static void attemptConnection()
{
  clearConnectionStatus(NETWORK_CONNECTING);

  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to request connection: %s", esp_err_to_name(err));
    clearConnectionStatus(NETWORK_OFFLINE);
    scheduleReconnect();
  }
}

static void processReconnect()
{
  if (!normal_connection_enabled || reconnect_started_us < 0) {
    return;
  }

  const int64_t now = esp_timer_get_time();
  if (now < reconnect_due_us || NetworkGetStatus().link != NETWORK_OFFLINE) {
    return;
  }

  // Retry every 10 seconds for the first minute, then every 30 seconds.
  if (now - reconnect_started_us < RECONNECT_FAST_PERIOD_US) {
    reconnect_due_us = now + RECONNECT_FAST_DELAY_US;
  } else {
    reconnect_due_us = now + RECONNECT_SLOW_DELAY_US;
  }
  attemptConnection();
}

static esp_err_t startSavedNetwork()
{
  esp_err_t err;

  err = esp_netif_set_hostname(station_interface, OperatingParameters.DeviceName);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set hostname: %s", esp_err_to_name(err));
    return err;
  }

  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set Wi-Fi mode: %s", esp_err_to_name(err));
    return err;
  }

  clearConnectionStatus(NETWORK_CONNECTING);

  err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start Wi-Fi: %s", esp_err_to_name(err));
    clearConnectionStatus(NETWORK_OFFLINE);
    return err;
  }

  return ESP_OK;
}

static void refreshRssi()
{
  const NETWORK_STATUS status = NetworkGetStatus();
  if (status.link != NETWORK_WAITING_FOR_IP && status.link != NETWORK_READY) {
    return;
  }

  wifi_ap_record_t ap_info = {};
  esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);

  portENTER_CRITICAL(&status_lock);
  current_status.rssi = (err == ESP_OK) ? ap_info.rssi : 0;
  current_status.rssi_valid = (err == ESP_OK);
  portEXIT_CRITICAL(&status_lock);
}

// Event-specific behavior.
static void handleStationStarted()
{
  if (normal_connection_enabled) {
    ESP_LOGI(TAG, "Wi-Fi started, attempting to connect to saved network");
    attemptConnection();
  }
}

static void handleStationConnected(const wifi_event_sta_connected_t *connected)
{
  ESP_LOGI(TAG, "Wi-Fi connected");

  size_t ssid_len = connected->ssid_len;
  if (ssid_len > sizeof(current_status.ssid) - 1) {
    ssid_len = sizeof(current_status.ssid) - 1;
  }

  setAssociatedStatus(connected->ssid, ssid_len);
  refreshRssi();
}

static void handleGotIp(const ip_event_got_ip_t *got_ip_event)
{
  ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&got_ip_event->ip_info.ip));
  setReadyStatus(got_ip_event->ip_info.ip);
  resetReconnect();
}

static void handleLostIp()
{
  ESP_LOGI(TAG, "Lost IP address");
  clearIpStatus();
}

static void handleDisconnected(const wifi_event_sta_disconnected_t *disconnected)
{
  uint8_t reason = disconnected->reason;
  ESP_LOGI(TAG, "Wi-Fi disconnected, reason: %d", reason);
  clearConnectionStatus(NETWORK_OFFLINE, reason);
  scheduleReconnect();
}

static void handleStationStopped()
{
  ESP_LOGI(TAG, "Wi-Fi stopped");
  normal_connection_enabled = false;
  resetReconnect();
  clearConnectionStatus(NETWORK_OFFLINE);
}

static void handleConnectSaved()
{
  if (normal_connection_enabled) {
    return;
  }

  resetReconnect();
  normal_connection_enabled = true;
  esp_err_t start_err = startSavedNetwork();
  if (start_err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start saved network: %s", esp_err_to_name(start_err));
    normal_connection_enabled = false;
    resetReconnect();
  }
}

static void handleNetworkTick()
{
  refreshRssi();
  processReconnect();
}

// Event dispatch: application requests, Wi-Fi lifecycle, and IP lifecycle.
static void eventHandler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  if (event_base == WIFI_EVENT) {
    switch (event_id) {
      case WIFI_EVENT_STA_START:
        handleStationStarted();
        break;
      case WIFI_EVENT_STA_CONNECTED:
        handleStationConnected(static_cast<const wifi_event_sta_connected_t *>(event_data));
        break;
      case WIFI_EVENT_STA_DISCONNECTED:
        handleDisconnected(static_cast<const wifi_event_sta_disconnected_t *>(event_data));
        break;
      case WIFI_EVENT_STA_STOP:
        handleStationStopped();
        break;
      default:
        break;
    }
  } else if (event_base == IP_EVENT) {
    switch (event_id) {
      case IP_EVENT_STA_GOT_IP:
        handleGotIp(static_cast<const ip_event_got_ip_t *>(event_data));
        break;
      case IP_EVENT_STA_LOST_IP:
        handleLostIp();
        break;
      default:
        break;
    }
  } else if (event_base == NETWORK_EVENT) {
    switch (event_id) {
      case NETWORK_EVENT_CONNECT_SAVED:
        handleConnectSaved();
        break;
      case NETWORK_EVENT_TICK:
        handleNetworkTick();
        break;
      default:
        break;
    }
  }
}

// The recurring timer only posts work; a dropped tick is retried next period.
static void networkTimerCallback(void *arg)
{
  (void)esp_event_post(NETWORK_EVENT, NETWORK_EVENT_TICK, nullptr, 0, 0);
}

// Resource initialization and public API.
esp_err_t NetworkInit()
{
  if (network_initialized) {
    return ESP_OK;
  }

  clearConnectionStatus(NETWORK_OFFLINE);

  bool event_loop_created = false;
  bool wifi_initialized = false;

  auto rollback = [&](esp_err_t original_error) -> esp_err_t {
    if (network_timer != nullptr) {
      ESP_ERROR_CHECK(esp_timer_delete(network_timer));
      network_timer = nullptr;
    }

    if (wifi_initialized) {
      ESP_ERROR_CHECK(esp_wifi_deinit());
    }

    if (station_interface != nullptr) {
      esp_netif_destroy_default_wifi(station_interface);
      station_interface = nullptr;
    }

    if (event_loop_created) {
      ESP_ERROR_CHECK(esp_event_loop_delete_default());
    }

    return original_error;
  };

  esp_err_t err = ESP_OK;
  if (!netif_initialized) {
    err = esp_netif_init();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to initialize network interface: %s", esp_err_to_name(err));
      return err;
    }
    netif_initialized = true;
  }

  err = esp_event_loop_create_default();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create default event loop: %s", esp_err_to_name(err));
    return rollback(err);
  }
  event_loop_created = true;

  station_interface = esp_netif_create_default_wifi_sta();
  if (!station_interface) {
    ESP_LOGE(TAG, "Failed to create default Wi-Fi station interface");
    return rollback(ESP_FAIL);
  }

  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize Wi-Fi: %s", esp_err_to_name(err));
    return rollback(err);
  }
  wifi_initialized = true;

  err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &eventHandler, nullptr, nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register Wi-Fi event handler: %s", esp_err_to_name(err));
    return rollback(err);
  }

  err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &eventHandler, nullptr, nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(err));
    return rollback(err);
  }

  err = esp_event_handler_instance_register(NETWORK_EVENT, ESP_EVENT_ANY_ID, &eventHandler, nullptr, nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register network event handler: %s", esp_err_to_name(err));
    return rollback(err);
  }

  esp_timer_create_args_t network_timer_args = {
    .callback = &networkTimerCallback,
    .arg = nullptr,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "network_tick",
    .skip_unhandled_events = true,
  };
  err = esp_timer_create(&network_timer_args, &network_timer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create network timer: %s", esp_err_to_name(err));
    return rollback(err);
  }

  err = esp_timer_start_periodic(
    network_timer,
    static_cast<uint64_t>(NETWORK_TICK_INTERVAL) * 1000ULL
  );
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start network timer: %s", esp_err_to_name(err));
    return rollback(err);
  }

  network_initialized = true;
  return ESP_OK;
}

esp_err_t NetworkConnectSaved() {
  if (!network_initialized) {
    ESP_LOGE(TAG, "Network not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t err = ESP_OK;

  wifi_config_t config = {};
  err = esp_wifi_get_config(WIFI_IF_STA, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get Wi-Fi config: %s", esp_err_to_name(err));
    return err;
  }
  if (config.sta.ssid[0] == '\0') {
    ESP_LOGI(TAG, "No saved Wi-Fi network found");
    return ESP_ERR_NOT_FOUND;
  }

  return esp_event_post(NETWORK_EVENT, NETWORK_EVENT_CONNECT_SAVED, nullptr, 0, 0);
}

NETWORK_STATUS NetworkGetStatus()
{
  portENTER_CRITICAL(&status_lock);
  NETWORK_STATUS status = current_status;
  portEXIT_CRITICAL(&status_lock);
  return status;
}

uint16_t NetworkRssiToPercent(int rssi)
{
  return static_cast<uint16_t>(std::clamp(2 * (rssi + 100), 0, 100));
}
