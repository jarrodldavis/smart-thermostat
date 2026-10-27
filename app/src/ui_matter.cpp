// SPDX-License-Identifier: GPL-3.0-only
#ifdef MATTER_ENABLED

#include "thermostat.hpp"
#include "lvgl.h"
#include "ui/ui.h"

// Runtime widgets keep commissioning controls out of generated screen files.
// All widget access happens on the LVGL task; Matter supplies a locked snapshot.
#if LV_USE_QRCODE
static lv_obj_t *pairingQr = nullptr;
#endif
static lv_obj_t *pairingStatus = nullptr;
static lv_obj_t *networkButton = nullptr;
static lv_obj_t *bleButton = nullptr;
static lv_obj_t *bleLabel = nullptr;
static lv_obj_t *stopButton = nullptr;
static lv_timer_t *pairingTimer = nullptr;
#if LV_USE_QRCODE
static char shownQr[128] = "";
#endif
static volatile bool showRequested = false;
static bool initialSetupScreen = false;

static void labelText(lv_obj_t *label, const char *text)
{
  if (strcmp(lv_label_get_text(label), text) != 0)
    lv_label_set_text(label, text);
}

static void enabled(lv_obj_t *button, bool value)
{
  if (value)
    lv_obj_clear_state(button, LV_STATE_DISABLED);
  else
    lv_obj_add_state(button, LV_STATE_DISABLED);
}

static void refreshPairing(lv_timer_t *)
{
  MATTER_PAIRING_STATE state = {};
  MatterGetPairingState(&state);
  const bool canChoose = state.started && !state.paired && !state.sessionActive;
  enabled(networkButton, canChoose && state.wifiConnected);
  enabled(bleButton, canChoose && state.bleSupported);
  if (state.bleSupported)
    lv_obj_clear_flag(bleButton, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(bleButton, LV_OBJ_FLAG_HIDDEN);
  enabled(stopButton, state.started && !state.paired && state.windowOpen);
  const bool bleActive = state.windowOpen && state.transport == MATTER_PAIRING_TRANSPORT::BLE;
  labelText(bleLabel, bleActive ? "BLE active" : !state.paired && state.bleRestartRequired ? "Restart\nfor BLE" : "BLE");
  const char *status = state.status;
  if (!state.started)
    status = OperatingParameters.MatterEnabled ? "Matter could not start. Restart the device." : "Enable Matter in Settings to begin setup.";
  else if (state.windowOpen && state.transport == MATTER_PAIRING_TRANSPORT::BLE && !state.wifiConnected)
    status = LV_USE_QRCODE ? "Scan in your Matter app to set up Wi-Fi and Matter together."
                           : "Enter the pairing code in your Matter app to set up Wi-Fi and Matter.";
  labelText(pairingStatus, status);

  if (state.showCode)
  {
#if LV_USE_QRCODE
    if (pairingQr == nullptr)
    {
      pairingQr = lv_qrcode_create(ui_MatterConfig, 120, lv_color_black(), lv_color_white());
      if (pairingQr == nullptr)
      {
        labelText(ui_ManualPairingCode, state.manual);
        return;
      }
      lv_obj_set_pos(pairingQr, 8, 28);
      lv_obj_set_style_border_color(pairingQr, lv_color_white(), 0);
      lv_obj_set_style_border_width(pairingQr, 4, 0);
    }
    if (strcmp(shownQr, state.qr) != 0)
    {
      if (lv_qrcode_update(pairingQr, state.qr, strlen(state.qr)) == LV_RES_OK)
        strlcpy(shownQr, state.qr, sizeof(shownQr));
      else
      {
        lv_obj_add_flag(pairingQr, LV_OBJ_FLAG_HIDDEN);
        labelText(ui_ManualPairingCode, state.manual);
        return;
      }
    }
    lv_obj_clear_flag(pairingQr, LV_OBJ_FLAG_HIDDEN);
#endif
    labelText(ui_ManualPairingCode, state.manual);
  }
  else
  {
#if LV_USE_QRCODE
    if (pairingQr != nullptr)
      lv_obj_add_flag(pairingQr, LV_OBJ_FLAG_HIDDEN);
#endif
    labelText(ui_ManualPairingCode, "");
  }
}

static void selectNetwork(lv_event_t *)
{
  MatterOpenCommissioningWindow(MATTER_PAIRING_TRANSPORT::NETWORK);
}

static void selectBle(lv_event_t *)
{
  MatterOpenCommissioningWindow(MATTER_PAIRING_TRANSPORT::BLE);
}

static void stopPairing(lv_event_t *)
{
  MatterCloseCommissioningWindow();
}

static lv_obj_t *button(const char *text, int y, lv_event_cb_t callback, lv_obj_t **labelOut = nullptr)
{
  lv_obj_t *btn = lv_btn_create(ui_MatterConfig);
  lv_obj_set_pos(btn, 215, y);
  lv_obj_set_size(btn, 95, 40);
  lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, nullptr);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x2095F6), 0);
  lv_obj_t *label = lv_label_create(btn);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(0xFFE300), 0);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  if (labelOut)
    *labelOut = label;
  return btn;
}

void UiMatterShowCommissioning()
{
  if (pairingStatus == nullptr)
  {
    lv_obj_add_flag(ui_QRImage, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *title = lv_label_create(ui_MatterConfig);
    lv_label_set_text(title, "Matter pairing");
    lv_obj_set_pos(title, 8, 2);
    networkButton = button("Network", 20, selectNetwork);
    bleButton = button("BLE", 66, selectBle, &bleLabel);
    stopButton = button("Stop", 112, stopPairing);
    pairingStatus = lv_label_create(ui_MatterConfig);
    lv_obj_set_pos(pairingStatus, 8, 157);
    lv_obj_set_size(pairingStatus, 304, 42);
    lv_obj_set_style_text_font(pairingStatus, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(pairingStatus, LV_LABEL_LONG_WRAP);
    pairingTimer = lv_timer_create(refreshPairing, 250, nullptr);
    lv_obj_add_event_cb(ui_MatterConfig, [](lv_event_t *) { lv_timer_pause(pairingTimer); },
                        LV_EVENT_SCREEN_UNLOADED, nullptr);
  }
  lv_timer_resume(pairingTimer);
  refreshPairing(nullptr);
}

void UiMatterRequestShow()
{
  showRequested = true;
}

void UiMatterShowIfRequested()
{
  if (showRequested)
  {
    showRequested = false;
    initialSetupScreen = true;
    _ui_screen_change(&ui_MatterConfig, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_MatterConfig_screen_init);
  }
  if (initialSetupScreen)
  {
    MATTER_PAIRING_STATE state = {};
    MatterGetPairingState(&state);
    if (state.paired && !state.sessionActive)
    {
      initialSetupScreen = false;
      if (lv_scr_act() == ui_MatterConfig)
        _ui_screen_change(&ui_MainScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_MainScreen_screen_init);
    }
  }
}

#endif
