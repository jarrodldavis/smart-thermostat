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
 *
 * All LVGL calls happen from LVGL timer callbacks / event handlers, i.e.
 * from the LVGL task. Other tasks only set a flag (UiProvRequestShow()).
 */

#include <stdio.h>
#include "lvgl.h"
#include "thermostat.hpp"
#include "ui/ui.h"

#define PROV_QR_SIZE 120

static lv_obj_t *provButton = NULL;
static lv_obj_t *provPanel = NULL;
static lv_obj_t *provInfoLabel = NULL;
static lv_obj_t *provQr = NULL;
static lv_timer_t *provTimer = NULL;
static char provQrShown[200] = "";
static volatile bool showRequested = false;
static volatile bool dismissRequested = false;

// Same look as the SquareLine-generated buttons
static void styleButton(lv_obj_t *btn, lv_obj_t *label)
{
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x2095F6), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_grad_color(btn, lv_color_hex(0x2620F6), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(label, lv_color_hex(0xFFE300), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_opa(label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
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
  styleButton(btn, label);
  return btn;
}

static void updateInfo()
{
  if (provInfoLabel == NULL)
    return;

  if (WifiProvActive())
  {
    lv_label_set_text_fmt(provInfoLabel, "%s\n%s", WifiProvServiceName(), WifiProvStatusText());

    // Only regenerate the QR code when the payload changes
    const char *payload = WifiProvQrPayload();
    if (provQr == NULL)
    {
      provQr = lv_qrcode_create(provPanel, PROV_QR_SIZE, lv_color_black(), lv_color_white());
      if (provQr == NULL)
      {
        lv_label_set_text(provInfoLabel, "Not enough memory for QR.\nStop and retry provisioning.");
        return;
      }
      lv_obj_align(provQr, LV_ALIGN_TOP_LEFT, 0, 0);
      lv_obj_set_style_border_color(provQr, lv_color_white(), 0);
      lv_obj_set_style_border_width(provQr, 4, 0);
    }
    if (strcmp(payload, provQrShown) != 0)
    {
      if (lv_qrcode_update(provQr, payload, strlen(payload)) != LV_RES_OK)
      {
        lv_obj_add_flag(provQr, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(provInfoLabel, "Could not render QR.\nStop and retry provisioning.");
        return;
      }
      lv_obj_clear_flag(provQr, LV_OBJ_FLAG_HIDDEN);
      strlcpy(provQrShown, payload, sizeof(provQrShown));
    }
  }
  else
  {
    if (provQr != NULL)
    {
      lv_obj_del(provQr);
      provQr = NULL;
      provQrShown[0] = '\0';
    }
    lv_label_set_text_fmt(provInfoLabel, "Wi-Fi provisioning\n%s\nChoose SoftAP", WifiProvStatusText());
  }
}

static void closePanel(lv_event_t *e)
{
  if (WifiProvActive())
    WifiProvStop();

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
    provQr = NULL;
    provQrShown[0] = '\0';
  }
}

static void provTimerCb(lv_timer_t *t)
{
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

static void openPanel(lv_event_t *e)
{
  if (provPanel != NULL)
    return;

  provPanel = lv_obj_create(ui_WifiConfig);
  lv_obj_set_size(provPanel, 320, 240);
  lv_obj_align(provPanel, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_clear_flag(provPanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(provPanel, 4, 0);

  // Info text sits under the QR code (which is created on demand)
  provInfoLabel = lv_label_create(provPanel);
  lv_obj_set_width(provInfoLabel, 200);
  lv_obj_align(provInfoLabel, LV_ALIGN_TOP_LEFT, 0, PROV_QR_SIZE + 8);

  addButton(provPanel, "SoftAP", LV_ALIGN_TOP_RIGHT, 0, 44, 90, startSoftAp);
  addButton(provPanel, "Stop", LV_ALIGN_TOP_RIGHT, 0, 88, 90, stopProv);
  addButton(provPanel, "Close", LV_ALIGN_BOTTOM_RIGHT, 0, 0, 90, closePanel);

  updateInfo();
  provTimer = lv_timer_create(provTimerCb, 500, NULL);
}

// Safe to call from any task
void UiProvRequestShow()
{
  showRequested = true;
}

// Safe to call from any task
void UiProvRequestDismiss()
{
  dismissRequested = true;
}

// Called from the LVGL task loop; opens or dismisses the panel if another task asked for it
void UiProvShowIfRequested()
{
  if (dismissRequested)
  {
    dismissRequested = false;
    showRequested = false;
    if (provPanel != NULL)
    {
      closePanel(NULL);
      if (lv_disp_get_scr_act(NULL) == ui_WifiConfig)
        _ui_screen_change(&ui_MainScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_MainScreen_screen_init);
    }
    return;
  }

  if (!showRequested)
    return;
  showRequested = false;

  _ui_screen_change(&ui_WifiConfig, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_WifiConfig_screen_init);
  openPanel(NULL);
}

// Must be called after ui_init(), from the LVGL task context
void UiProvInit()
{
  if (provButton != NULL)
    return;

  // Fits between the Scan button (x 1..101) and the SSID dropdown (x >= 155)
  provButton = lv_btn_create(ui_WifiConfig);
  lv_obj_set_size(provButton, 40, 36);
  lv_obj_align(provButton, LV_ALIGN_TOP_LEFT, 108, 1);
  lv_obj_add_flag(provButton, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_obj_add_event_cb(provButton, openPanel, LV_EVENT_CLICKED, NULL);
  lv_obj_t *label = lv_label_create(provButton);
  lv_label_set_text(label, "Prov");
  lv_obj_center(label);
  styleButton(provButton, label);
}
