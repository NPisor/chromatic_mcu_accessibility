#include "gbc_color_temp.h"

#include "line.h"
#include "lvgl.h"
#include "settings.h"

#include <string.h>

enum {
    kBarOffsetX_px = 83, // center bars without clipping
    kBarOffsetY_px = 60,
    kBarWidth_px   = 8,
    kBarGap_px     = 3,
    kBarHeight_px  = 26,
};

// Neutral at 0, then five warmer steps.
static const uint32_t kTempColors[kGBCColorTemp_MaxLevel + 1] = {
    0xCCCCCC, // neutral
    0xF7D6C2, // warm 1
    0xF2C2A3, // warm 2
    0xEEAE85, // warm 3
    0xE99966, // warm 4
    0xE5854A, // warm 5 (hottest)
};

static const char* kTempLabels[kGBCColorTemp_MaxLevel + 1] = {
    "NEUTRAL",
    "WARM 1",
    "WARM 2",
    "WARM 3",
    "WARM 4",
    "WARM 5",
};

typedef struct GBCColorTempCtx {
    lv_obj_t* pLabel;
    Line_t Bars[kGBCColorTemp_MaxLevel + 1];
    GBCColorTempLevel_t Level;
    fnOnUpdateCb_t fnOnUpdateCb;
    bool Initialized;
} GBCColorTempCtx_t;

static GBCColorTempCtx_t _Ctx;

static GBCColorTempLevel_t MapLegacyLevel(uint32_t raw)
{
    // Legacy scale was 0=coolest..4=warmest with neutral at 2. Collapse anything <=2 to neutral.
    if (raw <= 2u) {
        return kGBCColorTemp_MinLevel;
    }
    const uint32_t shifted = raw - 2u;
    return (GBCColorTempLevel_t)MIN(shifted, kGBCColorTemp_MaxLevel);
}

static void SaveToSettings(GBCColorTempLevel_t level);
static void DrawBars(lv_obj_t* pScreen, bool recalc);
static void EnsureInitialized(void);

OSD_Result_t GBCColorTemp_Draw(void* arg)
{
    if (arg == NULL)
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    lv_obj_t* const pScreen = (lv_obj_t *const)arg;

    EnsureInitialized();

    if (_Ctx.pLabel == NULL)
    {
        _Ctx.pLabel = lv_label_create(pScreen);
        lv_obj_add_style(_Ctx.pLabel, OSD_GetStyleTextWhite(), 0);
    }

    // Align label with left-pane text baseline while keeping it over the bars horizontally
    lv_obj_set_pos(_Ctx.pLabel, kBarOffsetX_px, kBarOffsetY_px - 10);
    lv_label_set_text_static(_Ctx.pLabel, kTempLabels[_Ctx.Level]);

    static GBCColorTempLevel_t prevLevel;
    const bool recalc = (prevLevel != _Ctx.Level);
    prevLevel = _Ctx.Level;

    DrawBars(pScreen, recalc);
    return kOSD_Result_Ok;
}

OSD_Result_t GBCColorTemp_OnButton(const Button_t Button, const ButtonState_t State, void* arg)
{
    (void)arg;

    if (State != kButtonState_Pressed)
    {
        return kOSD_Result_Ok;
    }

    switch (Button)
    {
        case kButton_Up:
            if (_Ctx.Level < kGBCColorTemp_MaxLevel)
            {
                _Ctx.Level++;
                SaveToSettings(_Ctx.Level);
                if (_Ctx.fnOnUpdateCb != NULL)
                {
                    _Ctx.fnOnUpdateCb();
                }
            }
            break;
        case kButton_Down:
            if (_Ctx.Level > kGBCColorTemp_MinLevel)
            {
                _Ctx.Level--;
                SaveToSettings(_Ctx.Level);
                if (_Ctx.fnOnUpdateCb != NULL)
                {
                    _Ctx.fnOnUpdateCb();
                }
            }
            break;
        default:
            break;
    }

    return kOSD_Result_Ok;
}

OSD_Result_t GBCColorTemp_OnTransition(void* arg)
{
    (void)arg;

    if (_Ctx.pLabel != NULL)
    {
        lv_obj_del(_Ctx.pLabel);
        _Ctx.pLabel = NULL;
    }

    for (size_t i = 0; i < ARRAY_SIZE(_Ctx.Bars); i++)
    {
        if (_Ctx.Bars[i].pObj != NULL)
        {
            lv_obj_del(_Ctx.Bars[i].pObj);
            _Ctx.Bars[i].pObj = NULL;
        }
    }

    return kOSD_Result_Ok;
}

void GBCColorTemp_InitFromSettings(void)
{
    EnsureInitialized();
}

GBCColorTempLevel_t GBCColorTemp_GetLevel(void)
{
    EnsureInitialized();
    return _Ctx.Level;
}

void GBCColorTemp_RegisterOnUpdateCb(fnOnUpdateCb_t fnOnUpdate)
{
    _Ctx.fnOnUpdateCb = fnOnUpdate;
}

OSD_Result_t GBCColorTemp_ApplySetting(const SettingValue_t* pValue)
{
    if (pValue == NULL)
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    if (pValue->eType != kSettingDataType_U8)
    {
        return kOSD_Result_Err_UnexpectedSettingDataType;
    }

    const GBCColorTempLevel_t level = MapLegacyLevel(pValue->U8);
    _Ctx.Level = level;
    _Ctx.Initialized = true;
    return kOSD_Result_Ok;
}

static void EnsureInitialized(void)
{
    if (_Ctx.Initialized)
    {
        return;
    }

    uint32_t stored = 0;
    if (Settings_Retrieve(kSettingKey_GBCColorTemp, &stored) == kOSD_Result_Ok)
    {
        _Ctx.Level = MapLegacyLevel(stored);
    }
    else
    {
        _Ctx.Level = kGBCColorTemp_MinLevel; // neutral default
    }

    _Ctx.Initialized = true;
}

static void DrawBars(lv_obj_t* pScreen, bool recalc)
{
    for (size_t i = 0; i < ARRAY_SIZE(_Ctx.Bars); i++)
    {
        if (recalc && (_Ctx.Bars[i].pObj != NULL))
        {
            lv_obj_del(_Ctx.Bars[i].pObj);
            _Ctx.Bars[i].pObj = NULL;
        }

        if (_Ctx.Bars[i].pObj == NULL)
        {
            _Ctx.Bars[i].pObj = lv_line_create(pScreen);
        }

        const lv_point_t pts[kLineConst_NumPoints] = {
            { .x = kBarOffsetX_px + (int32_t)i * (kBarWidth_px + kBarGap_px), .y = kBarOffsetY_px },
            { .x = kBarOffsetX_px + (int32_t)i * (kBarWidth_px + kBarGap_px), .y = kBarOffsetY_px + kBarHeight_px },
        };
        memcpy(_Ctx.Bars[i].points, pts, sizeof(pts));

        lv_line_set_points(_Ctx.Bars[i].pObj, _Ctx.Bars[i].points, kLineConst_NumPoints);
        lv_obj_set_style_line_width(_Ctx.Bars[i].pObj, kBarWidth_px, LV_PART_MAIN);
        const uint32_t color = kTempColors[i];
        lv_obj_set_style_line_color(_Ctx.Bars[i].pObj, lv_color_hex(color), LV_PART_MAIN);

        if (i > _Ctx.Level)
        {
            lv_obj_set_style_opa(_Ctx.Bars[i].pObj, LV_OPA_40, LV_PART_MAIN);
        }
        else
        {
            lv_obj_set_style_opa(_Ctx.Bars[i].pObj, LV_OPA_COVER, LV_PART_MAIN);
        }
    }
}

static void SaveToSettings(GBCColorTempLevel_t level)
{
    (void)Settings_Update(kSettingKey_GBCColorTemp, level);
}
