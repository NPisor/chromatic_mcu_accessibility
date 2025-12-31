#pragma once

#include "osd_shared.h"
#include "settings.h"

// Numbered from neutral (0) upward through warmer steps.
enum {
    kGBCColorTemp_MinLevel = 0,
    kGBCColorTemp_MaxLevel = 5,
};

typedef uint8_t GBCColorTempLevel_t;

OSD_Result_t GBCColorTemp_Draw(void* arg);
OSD_Result_t GBCColorTemp_OnButton(const Button_t Button, const ButtonState_t State, void* arg);
OSD_Result_t GBCColorTemp_OnTransition(void* arg);

void GBCColorTemp_InitFromSettings(void);
GBCColorTempLevel_t GBCColorTemp_GetLevel(void);
void GBCColorTemp_RegisterOnUpdateCb(fnOnUpdateCb_t fnOnUpdate);

// Helper for restoring persisted value without forcing a draw.
OSD_Result_t GBCColorTemp_ApplySetting(const SettingValue_t* pValue);
