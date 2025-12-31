#include "bt_advert.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ble_bridge.h"
#include "lvgl.h"
#include "mutex.h"
#include "osd_shared.h"
#include "esp_log.h"

static lv_obj_t *pStatusLabel = NULL;
static lv_obj_t *pHintLabel = NULL;
static BLEBridge_State_t _lastState = kBLEBridge_State_Stopped;
static char _proxy_status[48] = {0};
static const char *TAG = "BTAdvert";

static void render(void)
{
    if (pStatusLabel == NULL)
    {
        return;
    }

    const bool enabled = BLEBridge_IsEnabled();
    BLEBridge_State_t s = BLEBridge_GetState();
    const char *state_str = enabled ? "READY" : "OFF";
    switch (s)
    {
        case kBLEBridge_State_Advertising:
            state_str = "ADVERTISING";
            break;
        case kBLEBridge_State_Connected:
            state_str = "CONNECTED";
            break;
        default:
            break;
    }

    const int last_rc = BLEBridge_GetLastAdvError();
    const char *last_err = BLEBridge_GetLastAdvErrorStr();

    if (last_rc != 0)
    {
        lv_label_set_text_fmt(pStatusLabel, "BLE: %s\n(%s err %d/0x%02x %s)", enabled ? "ON" : "OFF", state_str, last_rc, last_rc & 0xFF, last_err);
    }
    else if (_proxy_status[0] != '\0')
    {
        lv_label_set_text_fmt(pStatusLabel, "BLE: %s\n(%s)\n%s", enabled ? "ON" : "OFF", state_str, _proxy_status);
    }
    else
    {
        lv_label_set_text_fmt(pStatusLabel, "BLE: %s\n(%s)", enabled ? "ON" : "OFF", state_str);
    }

    if (pHintLabel != NULL)
    {
        lv_label_set_text_fmt(pHintLabel, "A: toggle BLE\nB: send test proxy");
    }
}

static void on_state_change(BLEBridge_State_t s)
{
    _lastState = s;
    render();
}

OSD_Result_t BTAdvert_Draw(void *arg)
{
    if (arg == NULL)
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    lv_obj_t *const pScreen = (lv_obj_t *const)arg;

    if (pStatusLabel == NULL)
    {
        pStatusLabel = lv_label_create(pScreen);
        lv_obj_add_style(pStatusLabel, OSD_GetStyleTextWhite(), 0);
        lv_label_set_long_mode(pStatusLabel, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(pStatusLabel, 90);
        lv_obj_set_style_text_align(pStatusLabel, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_align(pStatusLabel, LV_ALIGN_TOP_RIGHT);
        lv_obj_set_pos(pStatusLabel, -15, 45);
    }

    if (pHintLabel == NULL)
    {
        pHintLabel = lv_label_create(pScreen);
        lv_obj_add_style(pHintLabel, OSD_GetStyleTextWhite(), 0);
        lv_label_set_long_mode(pHintLabel, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(pHintLabel, 80);
        lv_obj_set_style_text_align(pHintLabel, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_align(pHintLabel, LV_ALIGN_TOP_RIGHT);
        lv_obj_set_pos(pHintLabel, -15, 65);
    }

    BLEBridge_RegisterStateCb(on_state_change);
    render();
    return kOSD_Result_Ok;
}

OSD_Result_t BTAdvert_OnButton(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed)
    {
        return kOSD_Result_Ok;
    }

    if (Button == kButton_A)
    {
        const bool new_state = !BLEBridge_IsEnabled();
        BLEBridge_SetEnabled(new_state);
        ESP_LOGI(TAG, "BLE toggled %s", new_state ? "ON" : "OFF");
    }
    else if (Button == kButton_B)
    {
        static const uint8_t probe[] = "RA_TEST_REQ";
        int rc = BLEBridge_SendProxy(probe, sizeof(probe) - 1);
        snprintf(_proxy_status, sizeof(_proxy_status), "TEST send rc=%d", rc);
        ESP_LOGI(TAG, "BLE proxy test send rc=%d", rc);
    }

    render();
    return kOSD_Result_Ok;
}

OSD_Result_t BTAdvert_OnTransition(void *arg)
{
    (void)arg;
    lv_obj_t *objs[] = {pStatusLabel, pHintLabel};
    for (size_t i = 0; i < (sizeof(objs) / sizeof(objs[0])); i++)
    {
        if (objs[i] != NULL)
        {
            lv_obj_del(objs[i]);
            objs[i] = NULL;
        }
    }
    pStatusLabel = NULL;
    pHintLabel = NULL;
    BLEBridge_RegisterStateCb(NULL);
    _lastState = kBLEBridge_State_Stopped;
    memset(_proxy_status, 0, sizeof(_proxy_status));
    return kOSD_Result_Ok;
}
