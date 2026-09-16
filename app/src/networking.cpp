#include "thermostat.hpp"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include <algorithm>

static const char *TAG = "NETWORK";

static bool netif_initialized = false;
static bool network_initialized = false;
static esp_netif_t *station_interface = nullptr;
static esp_event_handler_instance_t wifi_event_handler_instance;
static esp_event_handler_instance_t ip_event_handler_instance;

static uint32_t connection_generation = 0;
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

static void networkEventHandler(
  void *arg,
  esp_event_base_t event_base,
  int32_t event_id,
  void *event_data)
{
  esp_err_t err;

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    ESP_LOGI(TAG, "Wi-Fi started, attempting to connect to saved network");
    err = esp_wifi_connect();
    if (err != ESP_OK) {
      portENTER_CRITICAL(&status_lock);
      current_status.link = NETWORK_OFFLINE;
      current_status.ip = NO_IP;
      current_status.disconnect_reason = NO_DISCONNECT_REASON;
      current_status.ssid[0] = '\0';
      current_status.rssi = 0;
      current_status.rssi_valid = false;
      portEXIT_CRITICAL(&status_lock);

      ESP_LOGE(TAG, "Failed to connect to Wi-Fi: %s", esp_err_to_name(err));
    }
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
    const auto *connected = static_cast<const wifi_event_sta_connected_t *>(event_data);
    size_t ssid_len = connected->ssid_len;
    if (ssid_len > sizeof(current_status.ssid) - 1) {
      ssid_len = sizeof(current_status.ssid) - 1;
    }

    portENTER_CRITICAL(&status_lock);
    ++connection_generation;
    current_status.link = NETWORK_WAITING_FOR_IP;
    current_status.ip = NO_IP;
    memcpy(current_status.ssid, connected->ssid, ssid_len);
    current_status.ssid[ssid_len] = '\0';
    current_status.rssi = 0;
    current_status.rssi_valid = false;
    portEXIT_CRITICAL(&status_lock);

    ESP_LOGI(TAG, "Wi-Fi connected");
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *got_ip_event = (ip_event_got_ip_t *)event_data;

    portENTER_CRITICAL(&status_lock);
    current_status.link = NETWORK_READY;
    current_status.ip = got_ip_event->ip_info.ip;
    current_status.disconnect_reason = NO_DISCONNECT_REASON;
    portEXIT_CRITICAL(&status_lock);

    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&got_ip_event->ip_info.ip));
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
    portENTER_CRITICAL(&status_lock);
    current_status.ip = NO_IP;
    if (current_status.link == NETWORK_READY) {
      current_status.link = NETWORK_WAITING_FOR_IP;
    }
    portEXIT_CRITICAL(&status_lock);

    ESP_LOGI(TAG, "Lost IP address");
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    uint8_t reason = ((wifi_event_sta_disconnected_t *)event_data)->reason;

    portENTER_CRITICAL(&status_lock);
    ++connection_generation;
    current_status.link = NETWORK_OFFLINE;
    current_status.ip = NO_IP;
    current_status.disconnect_reason = reason;
    current_status.ssid[0] = '\0';
    current_status.rssi = 0;
    current_status.rssi_valid = false;
    portEXIT_CRITICAL(&status_lock);

    ESP_LOGI(TAG, "Wi-Fi disconnected, reason: %d", reason);
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
    portENTER_CRITICAL(&status_lock);
    ++connection_generation;
    current_status.link = NETWORK_OFFLINE;
    current_status.ip = NO_IP;
    current_status.disconnect_reason = NO_DISCONNECT_REASON;
    current_status.ssid[0] = '\0';
    current_status.rssi = 0;
    current_status.rssi_valid = false;
    portEXIT_CRITICAL(&status_lock);

    ESP_LOGI(TAG, "Wi-Fi stopped");
  }
}

NETWORK_STATUS NetworkGetStatus()
{
  portENTER_CRITICAL(&status_lock);
  NETWORK_STATUS status = current_status;
  portEXIT_CRITICAL(&status_lock);
  return status;
}

esp_err_t NetworkInit()
{
  if (network_initialized) {
    return ESP_OK;
  }

  portENTER_CRITICAL(&status_lock);
  current_status.link = NETWORK_OFFLINE;
  current_status.ip = NO_IP;
  current_status.disconnect_reason = NO_DISCONNECT_REASON;
  current_status.ssid[0] = '\0';
  current_status.rssi = 0;
  current_status.rssi_valid = false;
  portEXIT_CRITICAL(&status_lock);

  bool event_loop_created = false;
  bool wifi_initialized = false;
  bool wifi_handler_registered = false;
  bool ip_handler_registered = false;

  auto rollback = [&](esp_err_t original_error) -> esp_err_t {
    if (ip_handler_registered) {
      ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        IP_EVENT,
        ESP_EVENT_ANY_ID,
        ip_event_handler_instance
      ));
      ip_event_handler_instance = nullptr;
    }

    if (wifi_handler_registered) {
      ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_event_handler_instance
      ));
      wifi_event_handler_instance = nullptr;
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

  err = esp_event_handler_instance_register(
    WIFI_EVENT,
    ESP_EVENT_ANY_ID,
    &networkEventHandler,
    nullptr,
    &wifi_event_handler_instance
  );
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register Wi-Fi event handler: %s", esp_err_to_name(err));
    return rollback(err);
  }
  wifi_handler_registered = true;

  err = esp_event_handler_instance_register(
    IP_EVENT,
    ESP_EVENT_ANY_ID,
    &networkEventHandler,
    nullptr,
    &ip_event_handler_instance
  );
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(err));
    return rollback(err);
  }
  ip_handler_registered = true;

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

  portENTER_CRITICAL(&status_lock);
  ++connection_generation;
  current_status.link = NETWORK_CONNECTING;
  current_status.ip = NO_IP;
  current_status.disconnect_reason = NO_DISCONNECT_REASON;
  current_status.ssid[0] = '\0';
  current_status.rssi = 0;
  current_status.rssi_valid = false;
  portEXIT_CRITICAL(&status_lock);

  err = esp_wifi_start();
  if (err != ESP_OK) {
    portENTER_CRITICAL(&status_lock);
    current_status.link = NETWORK_OFFLINE;
    current_status.ip = NO_IP;
    current_status.disconnect_reason = NO_DISCONNECT_REASON;
    current_status.ssid[0] = '\0';
    current_status.rssi = 0;
    current_status.rssi_valid = false;
    portEXIT_CRITICAL(&status_lock);

    ESP_LOGE(TAG, "Failed to start Wi-Fi: %s", esp_err_to_name(err));
    return err;
  }

  return ESP_OK;
}

void NetworkRefreshRssi()
{
  uint32_t generation;
  bool associated;

  portENTER_CRITICAL(&status_lock);
  generation = connection_generation;
  associated =
    current_status.link == NETWORK_WAITING_FOR_IP ||
    current_status.link == NETWORK_READY;
  portEXIT_CRITICAL(&status_lock);

  if (!associated)
  {
    return;
  }

  wifi_ap_record_t ap_info = {};
  esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);

  portENTER_CRITICAL(&status_lock);
  if (generation == connection_generation &&
      (current_status.link == NETWORK_WAITING_FOR_IP ||
       current_status.link == NETWORK_READY))
  {
    current_status.rssi = (err == ESP_OK) ? ap_info.rssi : 0;
    current_status.rssi_valid = (err == ESP_OK);
  }
  portEXIT_CRITICAL(&status_lock);
}

uint16_t NetworkRssiToPercent(int rssi)
{
  return static_cast<uint16_t>(std::clamp(2 * (rssi + 100), 0, 100));
}
