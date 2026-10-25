/*!
// SPDX-License-Identifier: GPL-3.0-only
*/
/*
 * ui_prov.cpp
 *
 * Wi-Fi provisioning controls for the touchscreen. These widgets are
 * created at runtime on top of the SquareLine-generated ui_WifiConfig
 * screen so that regenerating the SquareLine project (which overwrites
 * everything in src/ui/) does not remove them.
 */

#include <stdio.h>
#include "lvgl.h"
#include "thermostat.hpp"
#include "ui/ui.h"

static lv_obj_t *provButton = NULL;
static lv_obj_t *provPanel = NULL;
static lv_obj_t *provInfoLabel = NULL;
static lv_timer_t *provTimer = NULL;

static void updateInfo()
{
  if (provInfoLabel == NULL)
    return;
  if (WifiProvActive())
    lv_label_set_text_fmt(provInfoLabel,
                          "Wi-Fi provisioning\n\nDevice: %s\nPoP: %s\n\n%s\n\nUse the Espressif\nprovisioning app",
                          WifiProvServiceName(), WifiProvPop(), WifiProvStatusText());
  else
    lv_label_set_text_fmt(provInfoLabel,
                          "Wi-Fi provisioning\n\n%s\n\nChoose a method to\nstart provisioning",
                          WifiProvStatusText());
}

static void provTimerCb(lv_timer_t *t)
{
  updateInfo();
}

static void closePanel(lv_event_t *e)
{
  if (provTimer)
  {
    lv_timer_del(provTimer);
    provTimer = NULL;
  }
  if (provPanel)
  {
    lv_obj_del(provPanel);
    provPanel = NULL;
    provInfoLabel = NULL;
  }
}

static void startBle(lv_event_t *e)
{
  WifiProvStart(WIFI_PROV_TRANSPORT_BLE);
  updateInfo();
}

static void startSoftAp(lv_event_t *e)
{
  WifiProvStart(WIFI_PROV_TRANSPORT_SOFTAP);
  updateInfo();
}

static void stopProv(lv_event_t *e)
{
  WifiProvStop();
  updateInfo();
}

static lv_obj_t *addButton(lv_obj_t *parent, const char *text, lv_align_t align, lv_coord_t x, lv_coord_t y,
                           lv_coord_t w, lv_event_cb_t cb)
{
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, w, 36);
  lv_obj_align(btn, align, x, y);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return btn;
}

static void openPanel(lv_event_t *e)
{
  if (provPanel != NULL)
    return;

  provPanel = lv_obj_create(ui_WifiConfig);
  lv_obj_set_size(provPanel, 320, 240);
  lv_obj_align(provPanel, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_clear_flag(provPanel, LV_OBJ_FLAG_SCROLLABLE);

  provInfoLabel = lv_label_create(provPanel);
  lv_obj_align(provInfoLabel, LV_ALIGN_TOP_LEFT, 0, 0);

  if (WifiProvBleAvailable())
    addButton(provPanel, "BLE", LV_ALIGN_TOP_RIGHT, 0, 0, 100, startBle);
  addButton(provPanel, "SoftAP", LV_ALIGN_TOP_RIGHT, 0, 40, 100, startSoftAp);
  addButton(provPanel, "Stop", LV_ALIGN_TOP_RIGHT, 0, 80, 100, stopProv);
  addButton(provPanel, "Close", LV_ALIGN_BOTTOM_RIGHT, 0, 0, 100, closePanel);

  updateInfo();
  provTimer = lv_timer_create(provTimerCb, 500, NULL);
}

// Must be called after ui_init(), from the LVGL task context
void UiProvInit()
{
  if (provButton != NULL)
    return;

  // Fits in the gap between the Scan button and the SSID dropdown
  provButton = lv_btn_create(ui_WifiConfig);
  lv_obj_set_size(provButton, 50, 36);
  lv_obj_align(provButton, LV_ALIGN_TOP_LEFT, 104, 1);
  lv_obj_add_event_cb(provButton, openPanel, LV_EVENT_CLICKED, NULL);
  lv_obj_t *label = lv_label_create(provButton);
  lv_label_set_text(label, "Prov");
  lv_obj_center(label);
}
