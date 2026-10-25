/*!
// SPDX-License-Identifier: GPL-3.0-only
*/
/*
 * wifi_prov.cpp
 *
 * Wi-Fi provisioning using the ESP-IDF "wifi_provisioning" component.
 * Credentials can be supplied from a phone using the Espressif
 * "ESP SoftAP Prov" or "ESP BLE Provisioning" apps. Both the SoftAP (HTTP)
 * and BLE (NimBLE) transports are supported; BLE requires CONFIG_BT_ENABLED
 * and CONFIG_BT_NIMBLE_ENABLED in sdkconfig.
 */

#include <string.h>
#include <stdio.h>
#include "thermostat.hpp"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_NIMBLE_ENABLED)
#define WIFI_PROV_BLE_AVAILABLE 1
#include "wifi_provisioning/scheme_ble.h"
#endif

static const char *TAG = "WIFI_PROV";

#define PROV_TIMEOUT_US (5LL * 60 * 1000 * 1000)

static volatile bool provActive = false;
static bool provEventsRegistered = false;
static WIFI_PROV_TRANSPORT provTransport = WIFI_PROV_TRANSPORT_SOFTAP;
static esp_timer_handle_t provTimer = NULL;
static char provServiceName[24] = "";
static char provPop[12] = "";
static char provStatus[96] = "Idle";
static bool provCredsPending = false;
static char pendingSsid[sizeof(WifiCreds.ssid)];
static char pendingPass[sizeof(WifiCreds.password)];

static void provSetStatus(const char *text)
{
  strlcpy(provStatus, text, sizeof(provStatus));
}

static void provTimeoutCb(void *arg)
{
  ESP_LOGW(TAG, "Provisioning timed out");
  provSetStatus("Timed out");
  wifi_prov_mgr_stop_provisioning();
}

static void provEventHandler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
  if (base != WIFI_PROV_EVENT)
    return;

  switch (id)
  {
  case WIFI_PROV_START:
    ESP_LOGI(TAG, "Provisioning started");
    provSetStatus("Waiting for phone");
    break;

  case WIFI_PROV_CRED_RECV:
  {
    wifi_sta_config_t *cfg = (wifi_sta_config_t *)data;
    strlcpy(pendingSsid, (const char *)cfg->ssid, sizeof(pendingSsid));
    strlcpy(pendingPass, (const char *)cfg->password, sizeof(pendingPass));
    ESP_LOGI(TAG, "Received credentials for SSID \"%s\"", pendingSsid);
    provSetStatus("Connecting...");
    break;
  }

  case WIFI_PROV_CRED_FAIL:
  {
    wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)data;
    ESP_LOGE(TAG, "Provisioning failed: %s",
             (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "auth error" : "AP not found");
    provSetStatus((*reason == WIFI_PROV_STA_AUTH_ERROR) ? "Failed: bad password" : "Failed: network not found");
    // Allow the phone to try again
    wifi_prov_mgr_reset_sm_state_on_failure();
    break;
  }

  case WIFI_PROV_CRED_SUCCESS:
    ESP_LOGI(TAG, "Provisioning successful");
    provSetStatus("Success");
    provCredsPending = true;
    break;

  case WIFI_PROV_END:
    ESP_LOGI(TAG, "Provisioning ended");
    if (provTimer)
      esp_timer_stop(provTimer);
    wifi_prov_mgr_deinit();
    provActive = false;
    if (provCredsPending)
    {
      provCredsPending = false;
      strlcpy(WifiCreds.ssid, pendingSsid, sizeof(WifiCreds.ssid));
      strlcpy(WifiCreds.password, pendingPass, sizeof(WifiCreds.password));
      setWifiCreds();
      WifiSetCredentials(WifiCreds.ssid, WifiCreds.password);
      // Reconnect using the normal path so connection status is tracked
      WifiDisconnect();
      lastWifiReconnect = 0;
    }
    break;

  default:
    break;
  }
}

bool WifiProvBleAvailable()
{
#ifdef WIFI_PROV_BLE_AVAILABLE
  return true;
#else
  return false;
#endif
}

bool WifiProvActive()
{
  return provActive;
}

const char *WifiProvStatusText()
{
  return provStatus;
}

const char *WifiProvServiceName()
{
  return provServiceName;
}

const char *WifiProvPop()
{
  return provPop;
}

bool WifiProvStart(WIFI_PROV_TRANSPORT transport)
{
  if (provActive)
  {
    ESP_LOGW(TAG, "Provisioning already active");
    return false;
  }
  if (!WifiStarted())
  {
    ESP_LOGE(TAG, "Wifi not started; cannot provision");
    provSetStatus("Failed: wifi not started");
    return false;
  }

#ifndef WIFI_PROV_BLE_AVAILABLE
  if (transport == WIFI_PROV_TRANSPORT_BLE)
  {
    ESP_LOGW(TAG, "BLE not enabled in sdkconfig; using SoftAP");
    transport = WIFI_PROV_TRANSPORT_SOFTAP;
  }
#endif

  if (!provEventsRegistered)
  {
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &provEventHandler, NULL));
    provEventsRegistered = true;
  }

  wifi_prov_mgr_config_t config = {};
#ifdef WIFI_PROV_BLE_AVAILABLE
  if (transport == WIFI_PROV_TRANSPORT_BLE)
  {
    config.scheme = wifi_prov_scheme_ble;
    config.scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM;
  }
  else
#endif
  {
    config.scheme = wifi_prov_scheme_softap;
    config.scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE;
  }

  esp_err_t err = wifi_prov_mgr_init(config);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "wifi_prov_mgr_init failed: %s", esp_err_to_name(err));
    provSetStatus("Failed: init error");
    return false;
  }

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(provServiceName, sizeof(provServiceName), "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);
  // Proof of possession; doubles as the SoftAP password (min. 8 chars)
  snprintf(provPop, sizeof(provPop), "TH%02X%02X%02X%02X", mac[2], mac[3], mac[4], mac[5]);

#ifdef WIFI_PROV_BLE_AVAILABLE
  if (transport == WIFI_PROV_TRANSPORT_BLE)
  {
    static uint8_t uuid[16] = {0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
                               0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02};
    wifi_prov_scheme_ble_set_service_uuid(uuid);
  }
#endif

  const char *serviceKey = (transport == WIFI_PROV_TRANSPORT_SOFTAP) ? provPop : NULL;
  err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, provPop, provServiceName, serviceKey);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "wifi_prov_mgr_start_provisioning failed: %s", esp_err_to_name(err));
    wifi_prov_mgr_deinit();
    provSetStatus("Failed: start error");
    return false;
  }

  provTransport = transport;
  provActive = true;
  provCredsPending = false;
  provSetStatus("Waiting for phone");

  if (provTimer == NULL)
  {
    esp_timer_create_args_t args = {};
    args.callback = provTimeoutCb;
    args.name = "prov_timeout";
    esp_timer_create(&args, &provTimer);
  }
  if (provTimer)
    esp_timer_start_once(provTimer, PROV_TIMEOUT_US);

  ESP_LOGI(TAG, "Provisioning via %s: service \"%s\", PoP \"%s\"",
           transport == WIFI_PROV_TRANSPORT_BLE ? "BLE" : "SoftAP", provServiceName, provPop);
  return true;
}

void WifiProvStop()
{
  if (provActive)
  {
    provSetStatus("Idle");
    wifi_prov_mgr_stop_provisioning();
  }
}
