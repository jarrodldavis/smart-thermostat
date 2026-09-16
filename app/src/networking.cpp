#include "thermostat.hpp"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "NETWORK";

static bool netif_initialized = false;
static bool network_initialized = false;
static esp_netif_t *station_interface = nullptr;
static esp_event_handler_instance_t wifi_event_handler_instance;
static esp_event_handler_instance_t ip_event_handler_instance;

static void networkEventHandler(
  void *arg,
  esp_event_base_t event_base,
  int32_t event_id,
  void *event_data)
{
  // Handle network events here
}

esp_err_t NetworkInit()
{
  if (network_initialized) {
    return ESP_OK;
  }

  bool event_loop_created = false;
  bool wifi_initialized = false;
  bool wifi_handler_registered = false;
  bool ip_handler_registered = false;

  auto rollback = [&](esp_err_t original_error) -> esp_err_t {
    if (ip_handler_registered) {
      ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
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
    IP_EVENT_STA_GOT_IP,
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
