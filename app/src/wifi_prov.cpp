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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "wifi_provisioning/manager.h"
#include "esp_netif.h"
#include "protocomm_security.h"
#include "wifi_provisioning/scheme_softap.h"
#ifdef CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2
#define WIFI_PROV_USE_SEC2 1
#include "esp_srp.h"
#endif
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_NIMBLE_ENABLED)
#define WIFI_PROV_BLE_AVAILABLE 1
#include "wifi_provisioning/scheme_ble.h"
#endif

static const char *TAG = "WIFI_PROV";

#define PROV_TIMEOUT_US (5LL * 60 * 1000 * 1000)

static volatile bool provActive = false;
static bool provEventsRegistered = false;
static esp_netif_t *provApNetif = NULL;
static WIFI_PROV_TRANSPORT provTransport = WIFI_PROV_TRANSPORT_SOFTAP;
static esp_timer_handle_t provTimer = NULL;
static char provServiceName[24] = "";
static char provSecret[9] = "";     // Security 2 password (Security 1 PoP)
static char provSoftApKey[9] = "";  // SoftAP Wi-Fi password
static char provQr[200] = "";
static char provStatus[96] = "Idle";
static bool provCredsPending = false;
static char pendingSsid[sizeof(WifiCreds.ssid)];
static char pendingPass[sizeof(WifiCreds.password)];
#ifdef WIFI_PROV_USE_SEC2
#define PROV_USERNAME "wifiprov"
static wifi_prov_security2_params_t provSec2Params;
static char *provSalt = NULL;
static char *provVerifier = NULL;
#else
#define PROV_USERNAME ""
#endif

static void provLogHeap(const char *when)
{
  ESP_LOGI(TAG, "Heap (%s): free=%u min_free=%u largest_internal_block=%u", when,
           (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// Random string from an alphabet without look-alike characters
static void provRandomString(char *out, size_t len)
{
  static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  uint8_t rnd[16];
  esp_fill_random(rnd, len);
  for (size_t i = 0; i < len; i++)
    out[i] = alphabet[rnd[i] % (sizeof(alphabet) - 1)];
  out[len] = '\0';
}

static void provFreeSecurity()
{
#ifdef WIFI_PROV_USE_SEC2
  free(provSalt);
  free(provVerifier);
  provSalt = NULL;
  provVerifier = NULL;
  memset(&provSec2Params, 0, sizeof(provSec2Params));
#endif
  memset(provSecret, 0, sizeof(provSecret));
  memset(provSoftApKey, 0, sizeof(provSoftApKey));
  provQr[0] = '\0';
}

static void provSetStatus(const char *text)
{
  strlcpy(provStatus, text, sizeof(provStatus));
}

extern bool wifiScanActive;

static void provEventHandler(void *arg, esp_event_base_t base, int32_t id, void *data);

// Single teardown path used by every end/failure case, in a fixed order.
// Must not run inside a provisioning/session event handler: unregistering the
// last handler of an event base from within its own dispatch frees the node
// that esp_event is still iterating, which crashes the system event task.
static void provTeardown(bool deinitManager = true)
{
  if (provTimer)
    esp_timer_stop(provTimer);
  if (deinitManager)
    wifi_prov_mgr_deinit();
  provFreeSecurity();
  if (provEventsRegistered)
  {
    esp_err_t err = esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &provEventHandler);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
      ESP_LOGW(TAG, "Failed to unregister event handler: %s", esp_err_to_name(err));
    err = esp_event_handler_unregister(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID, &provEventHandler);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
      ESP_LOGW(TAG, "Failed to unregister session event handler: %s", esp_err_to_name(err));
    provEventsRegistered = false;
  }
  // The manager may have left the radio in AP+STA mode
  esp_wifi_set_mode(WIFI_MODE_STA);
  if (provApNetif != NULL)
  {
    esp_netif_destroy_default_wifi(provApNetif);
    provApNetif = NULL;
  }
  provLogHeap("after provisioning");
}

// Provisioning finished without new credentials: release the wifi module and
// let it reconnect with whatever is stored. provActive must be cleared before
// the wifi module is told, since it gates the state machine's reconnect logic.
static void provEndWithoutCreds()
{
  provActive = false;
  WifiProvAborted();
}

// Runs after WIFI_PROV_END in its own task, outside the event dispatch: it
// unregisters the event handlers, and saving credentials / starting SNTP need
// far more stack than the system event task provides. provActive stays set
// until the end so the state machine cannot start a reconnect meanwhile.
static void provFinishTask(void *arg)
{
  provTeardown(false);
  if (provCredsPending)
  {
    provCredsPending = false;
    strlcpy(WifiCreds.ssid, pendingSsid, sizeof(WifiCreds.ssid));
    strlcpy(WifiCreds.password, pendingPass, sizeof(WifiCreds.password));
    setWifiCreds();
    // The station is already connected; keep that connection
    WifiProvisioned();
    provActive = false;
    UiProvRequestDismiss();
  }
  else
  {
    provEndWithoutCreds();
  }
  vTaskDelete(NULL);
}

static void provTimeoutCb(void *arg)
{
  ESP_LOGW(TAG, "Provisioning timed out");
  provSetStatus("Timed out");
  wifi_prov_mgr_stop_provisioning();
}

static void provEventHandler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
  if (base == PROTOCOMM_SECURITY_SESSION_EVENT)
  {
    if (id == PROTOCOMM_SECURITY_SESSION_SETUP_OK)
    {
      ESP_LOGI(TAG, "Phone connected");
      provSetStatus("Phone connected");
    }
    else if (id == PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH)
    {
      ESP_LOGW(TAG, "Phone used wrong credentials");
      provSetStatus("Phone: wrong credentials");
    }
    return;
  }

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
    // The manager must be deinitialised here (as ESP-IDF documents); the rest
    // of the cleanup is deferred to a task
    wifi_prov_mgr_deinit();
    if (xTaskCreate(provFinishTask, "prov_finish", 6144, NULL, 5, NULL) != pdPASS)
    {
      // Out of memory: release the wifi module so nothing stays gated on
      // provisioning. The event handlers stay registered and are reused by
      // the next WifiProvStart.
      ESP_LOGE(TAG, "Could not create prov_finish task; credentials not saved");
      provCredsPending = false;
      provFreeSecurity();
      provEndWithoutCreds();
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

const char *WifiProvQrPayload()
{
  return provQr;
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
  if (wifiScanActive)
  {
    ESP_LOGW(TAG, "Wifi scan active; cannot provision");
    provSetStatus("Failed: scan in progress");
    return false;
  }

  provLogHeap("before provisioning");

  // Set before the manager runs so the wifi module ignores the disconnect
  // that starting provisioning causes
  provActive = true;

#ifndef WIFI_PROV_BLE_AVAILABLE
  if (transport == WIFI_PROV_TRANSPORT_BLE)
  {
    ESP_LOGW(TAG, "BLE not enabled in sdkconfig; using SoftAP");
    transport = WIFI_PROV_TRANSPORT_SOFTAP;
  }
#endif

  if (!provEventsRegistered)
  {
    esp_err_t regErr = esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &provEventHandler, NULL);
    if (regErr == ESP_OK)
      regErr = esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID, &provEventHandler, NULL);
    provEventsRegistered = true; // teardown unregisters whichever succeeded
    if (regErr != ESP_OK)
    {
      ESP_LOGE(TAG, "Failed to register event handler: %s", esp_err_to_name(regErr));
      provTeardown();
      provSetStatus("Failed: init error");
      provEndWithoutCreds();
      return false;
    }
  }

  if (transport == WIFI_PROV_TRANSPORT_SOFTAP)
  {
    // The SoftAP scheme needs an AP netif (DHCP server) which the wifi module doesn't create
    provApNetif = esp_netif_create_default_wifi_ap();
    if (provApNetif == NULL)
    {
      ESP_LOGE(TAG, "Failed to create AP netif");
      provTeardown();
      provSetStatus("Failed: init error");
      provEndWithoutCreds();
      return false;
    }
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
    provTeardown();
    provSetStatus("Failed: init error");
    provEndWithoutCreds();
    return false;
  }

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(provServiceName, sizeof(provServiceName), "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);
  // Secrets are random and unrelated to the (public) service name / MAC
  provRandomString(provSecret, 8);
  provRandomString(provSoftApKey, 8);

#ifdef WIFI_PROV_USE_SEC2
  int verifierLen = 0;
  err = esp_srp_gen_salt_verifier(PROV_USERNAME, strlen(PROV_USERNAME), provSecret, strlen(provSecret),
                                  &provSalt, 16, &provVerifier, &verifierLen);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "esp_srp_gen_salt_verifier failed: %s", esp_err_to_name(err));
    provTeardown();
    provSetStatus("Failed: security error");
    provEndWithoutCreds();
    return false;
  }
  provSec2Params.salt = provSalt;
  provSec2Params.salt_len = 16;
  provSec2Params.verifier = provVerifier;
  provSec2Params.verifier_len = verifierLen;
  wifi_prov_security_t security = WIFI_PROV_SECURITY_2;
  const void *securityParams = &provSec2Params;
#else
  wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
  const void *securityParams = provSecret;
#endif

#ifdef WIFI_PROV_BLE_AVAILABLE
  if (transport == WIFI_PROV_TRANSPORT_BLE)
  {
    static uint8_t uuid[16] = {0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
                               0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02};
    wifi_prov_scheme_ble_set_service_uuid(uuid);
  }
#endif

  const char *serviceKey = (transport == WIFI_PROV_TRANSPORT_SOFTAP) ? provSoftApKey : NULL;
  if (transport == WIFI_PROV_TRANSPORT_SOFTAP)
  {
    // Port 80 is already used by the thermostat web server; share it
    void *webServer = webServerGetHandle();
    if (webServer != NULL)
      wifi_prov_scheme_softap_set_httpd_handle(webServer);
  }
  err = wifi_prov_mgr_start_provisioning(security, securityParams, provServiceName, serviceKey);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "wifi_prov_mgr_start_provisioning failed: %s", esp_err_to_name(err));
    provTeardown();
    provSetStatus("Failed: start error");
    provEndWithoutCreds();
    return false;
  }

  // Payload understood by the Espressif provisioning apps
  const char *transportName = (transport == WIFI_PROV_TRANSPORT_BLE) ? "ble" : "softap";
  int n = snprintf(provQr, sizeof(provQr),
                   "{\"ver\":\"v1\",\"name\":\"%s\",\"username\":\"%s\",\"pop\":\"%s\",\"transport\":\"%s\"",
                   provServiceName, PROV_USERNAME, provSecret, transportName);
  if (transport == WIFI_PROV_TRANSPORT_SOFTAP)
    n += snprintf(provQr + n, sizeof(provQr) - n, ",\"password\":\"%s\"", provSoftApKey);
  snprintf(provQr + n, sizeof(provQr) - n, "}");

  provTransport = transport;
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

  ESP_LOGI(TAG, "Provisioning via %s: service \"%s\"",
           transport == WIFI_PROV_TRANSPORT_BLE ? "BLE" : "SoftAP", provServiceName);
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
