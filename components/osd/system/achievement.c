#include "achievement.h"

#include "osd_shared.h"
#include "lvgl.h"
#include "esp_heap_caps.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum {
    kRootW_px = 150,
    kRootH_px = 116,
    // Place near the top-right of the pane with a small inset.
    kRootOffsetX_px = 25,
    kRootOffsetY_px = 28,

    kUsernameY_px = 12,
    kPointsY_px = 72,

    kMaxUsernameLen = 31,

    kAvatarScaleDefaultPct = 50,
    kAvatarScaleMinPct = 10,
    kAvatarScaleMaxPct = 100,
};

typedef struct AchievementCardCtx {
    lv_obj_t *pRoot;
    lv_obj_t *pAvatarImg;
    lv_obj_t *pAvatarFallback;
    lv_obj_t *pUsernameLabel;
    lv_obj_t *pPointsLabel;

    char Username[kMaxUsernameLen + 1];
    uint32_t Points;
    const lv_img_dsc_t *pAvatarSrc;
    uint8_t AvatarScalePct;
    uint16_t AvatarZoom;
    bool Dirty;
} AchievementCardCtx_t;

enum { kAvatarMaxDim_px = 128, kAvatarMaxBytes = kAvatarMaxDim_px * kAvatarMaxDim_px * 4 }; // 128x128 RGBA

// Retained buffer for the last avatar pushed by the phone. True-color RGBA.
static lv_img_dsc_t sAvatarDsc = {0};
static uint8_t *sAvatarBuffer = NULL;
static size_t sAvatarCapacity = 0;

static AchievementCardCtx_t sCardCtx = {
    .Username = "CONNECT PHONE",
    .AvatarScalePct = kAvatarScaleDefaultPct,
    .AvatarZoom = (uint16_t)((LV_IMG_ZOOM_NONE * kAvatarScaleDefaultPct) / 100U),
    .Dirty = true,
};

static bool ensure_avatar_buffer(void)
{
    if ((sAvatarBuffer != NULL) && (sAvatarCapacity >= kAvatarMaxBytes))
    {
        return true;
    }

    if (sAvatarBuffer != NULL)
    {
        heap_caps_free(sAvatarBuffer);
        sAvatarBuffer = NULL;
        sAvatarCapacity = 0;
    }

    sAvatarBuffer = (uint8_t *)heap_caps_malloc(kAvatarMaxBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (sAvatarBuffer == NULL)
    {
        sAvatarBuffer = (uint8_t *)heap_caps_malloc(kAvatarMaxBytes, MALLOC_CAP_8BIT);
    }

    if (sAvatarBuffer == NULL)
    {
        return false;
    }

    sAvatarCapacity = kAvatarMaxBytes;
    return true;
}

static uint16_t avatar_scale_to_zoom(const uint8_t scalePct)
{
    const uint8_t clamped = MIN(kAvatarScaleMaxPct, MAX(kAvatarScaleMinPct, scalePct));
    return (uint16_t)((LV_IMG_ZOOM_NONE * clamped) / 100U);
}

OSD_Result_t Achievement_GetAvatarScratch(uint8_t **ppBuf, size_t *pCapacity)
{
    if ((ppBuf == NULL) || (pCapacity == NULL))
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    if (!ensure_avatar_buffer())
    {
        return kOSD_Result_Err_UnexpectedSettingDataType;
    }

    *ppBuf = sAvatarBuffer;
    *pCapacity = sAvatarCapacity;
    return kOSD_Result_Ok;
}

static void set_avatar_visuals(void)
{
    if (sCardCtx.pAvatarImg == NULL)
    {
        return;
    }

    if (sCardCtx.pAvatarSrc != NULL)
    {
        lv_img_set_src(sCardCtx.pAvatarImg, sCardCtx.pAvatarSrc);
        lv_img_set_zoom(sCardCtx.pAvatarImg, sCardCtx.AvatarZoom);
        lv_obj_clear_flag(sCardCtx.pAvatarImg, LV_OBJ_FLAG_HIDDEN);

        if (sCardCtx.pAvatarFallback != NULL) lv_obj_add_flag(sCardCtx.pAvatarFallback, LV_OBJ_FLAG_HIDDEN);
    }
    else if (sCardCtx.pAvatarFallback != NULL)
    {
        // Show a simple placeholder when no avatar has been synced yet.
        const char initial = (sCardCtx.Username[0] != '\0') ? sCardCtx.Username[0] : 'U';
        char initials[2] = { initial, '\0' };
        lv_label_set_text(sCardCtx.pAvatarFallback, initials);
        lv_obj_clear_flag(sCardCtx.pAvatarFallback, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(sCardCtx.pAvatarImg, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_card(void)
{
    if (!sCardCtx.Dirty)
    {
        return;
    }

    if (sCardCtx.pUsernameLabel != NULL)
    {
        lv_label_set_text(sCardCtx.pUsernameLabel, sCardCtx.Username);
    }

    if (sCardCtx.pPointsLabel != NULL)
    {
        lv_label_set_text_fmt(sCardCtx.pPointsLabel, "%lu pts", (unsigned long)sCardCtx.Points);
    }

    set_avatar_visuals();
    sCardCtx.Dirty = false;
}

static void set_loaded_styles(void)
{
    // When profile data is loaded, switch text to green.
    if (sCardCtx.pUsernameLabel != NULL)
    {
        lv_obj_remove_style(sCardCtx.pUsernameLabel, OSD_GetStyleTextGrey(), 0);
        lv_obj_add_style(sCardCtx.pUsernameLabel, OSD_GetStyleTextWhite(), 0);
    }

    if (sCardCtx.pPointsLabel != NULL)
    {
        lv_obj_remove_style(sCardCtx.pPointsLabel, OSD_GetStyleTextGrey(), 0);
        lv_obj_add_style(sCardCtx.pPointsLabel, OSD_GetStyleTextWhite(), 0);
    }
}

static void ensure_card(lv_obj_t *const pScreen)
{
    if (sCardCtx.pRoot == NULL)
    {
        sCardCtx.pRoot = lv_obj_create(pScreen);
        lv_obj_remove_style_all(sCardCtx.pRoot);
        lv_obj_set_size(sCardCtx.pRoot, kRootW_px, kRootH_px);
        lv_obj_align(sCardCtx.pRoot, LV_ALIGN_TOP_RIGHT, kRootOffsetX_px, kRootOffsetY_px);
        lv_obj_set_style_bg_opa(sCardCtx.pRoot, LV_OPA_TRANSP, 0);
    }

    if (sCardCtx.pAvatarImg == NULL)
    {
        sCardCtx.pAvatarImg = lv_img_create(sCardCtx.pRoot);
        lv_obj_align(sCardCtx.pAvatarImg, LV_ALIGN_CENTER, 0, -12);
        lv_obj_add_flag(sCardCtx.pAvatarImg, LV_OBJ_FLAG_HIDDEN);
    }

    if (sCardCtx.pAvatarFallback == NULL)
    {
        sCardCtx.pAvatarFallback = lv_label_create(sCardCtx.pRoot);
        lv_obj_add_style(sCardCtx.pAvatarFallback, OSD_GetStyleTextGrey(), 0);
        lv_obj_align(sCardCtx.pAvatarFallback, LV_ALIGN_CENTER, 0, -12);
    }

    if (sCardCtx.pUsernameLabel == NULL)
    {
        sCardCtx.pUsernameLabel = lv_label_create(sCardCtx.pRoot);
        lv_obj_add_style(sCardCtx.pUsernameLabel, OSD_GetStyleTextGrey(), 0);
        lv_obj_align(sCardCtx.pUsernameLabel, LV_ALIGN_TOP_MID, 0, kUsernameY_px);
    }

    if (sCardCtx.pPointsLabel == NULL)
    {
        sCardCtx.pPointsLabel = lv_label_create(sCardCtx.pRoot);
        lv_obj_add_style(sCardCtx.pPointsLabel, OSD_GetStyleTextGrey(), 0);
        lv_obj_align(sCardCtx.pPointsLabel, LV_ALIGN_TOP_MID, 0, kPointsY_px);
    }
}

OSD_Result_t Achievement_Draw(void *arg)
{
    if (arg == NULL)
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    lv_obj_t *const pScreen = (lv_obj_t *)arg;

    ensure_card(pScreen);
    refresh_card();

    return kOSD_Result_Ok;
}

OSD_Result_t Achievement_OnTransition(void *arg)
{
    (void)arg;

    lv_obj_t **toDelete[] = {
        &sCardCtx.pAvatarImg,
        &sCardCtx.pAvatarFallback,
        &sCardCtx.pUsernameLabel,
        &sCardCtx.pPointsLabel,
        &sCardCtx.pRoot,
    };

    for (size_t i = 0; i < ARRAY_SIZE(toDelete); i++)
    {
        if (*toDelete[i] != NULL)
        {
            lv_obj_del(*toDelete[i]);
            *toDelete[i] = NULL;
        }
    }

    sCardCtx.Dirty = true;

    return kOSD_Result_Ok;
}

OSD_Result_t Achievement_OnButton(const Button_t button, const ButtonState_t state, void *arg)
{
    (void)button;
    (void)state;
    (void)arg;
    return kOSD_Result_Ok;
}

OSD_Result_t Achievement_SetUserProfile(const AchievementUserProfile_t *pProfile)
{
    if (pProfile == NULL)
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    const char *pName = (pProfile->pUsername != NULL) ? pProfile->pUsername : "UNKNOWN";
    strncpy(sCardCtx.Username, pName, kMaxUsernameLen);
    sCardCtx.Username[kMaxUsernameLen] = '\0';

    sCardCtx.Points = pProfile->Points;
    sCardCtx.pAvatarSrc = pProfile->pAvatar;

    const uint8_t scalePct = (pProfile->AvatarScalePct == 0U) ? kAvatarScaleDefaultPct : pProfile->AvatarScalePct;
    sCardCtx.AvatarScalePct = MIN(kAvatarScaleMaxPct, MAX(kAvatarScaleMinPct, scalePct));
    sCardCtx.AvatarZoom = avatar_scale_to_zoom(sCardCtx.AvatarScalePct);
    sCardCtx.Dirty = true;

    set_loaded_styles();

    // If UI already exists, refresh immediately to avoid stale data.
    refresh_card();

    return kOSD_Result_Ok;
}

OSD_Result_t Achievement_UpdateFromRawImage(const char *pUsername,
                                            uint32_t points,
                                            const uint8_t *pImgData,
                                            size_t imgDataLen,
                                            uint16_t width,
                                            uint16_t height,
                                            uint8_t avatarScalePct)
{
    if ((width == 0U) || (height == 0U) || (width > kAvatarMaxDim_px) || (height > kAvatarMaxDim_px))
    {
        return kOSD_Result_Err_UnexpectedSettingDataType;
    }

    const size_t rawRequired = (size_t)width * (size_t)height * 4U; // incoming RGBA8888
    const size_t required = LV_IMG_BUF_SIZE_TRUE_COLOR_ALPHA(width, height); // LVGL-native format

    if ((pImgData == NULL) || (imgDataLen < rawRequired))
    {
        return kOSD_Result_Err_UnexpectedSettingDataType;
    }

    if (!ensure_avatar_buffer() || (required > sAvatarCapacity))
    {
        return kOSD_Result_Err_UnexpectedSettingDataType;
    }

#if (LV_COLOR_DEPTH == 16)
    // Convert incoming RGBA8888 into LVGL's 16-bit color + 8-bit alpha format (RGB565 + A8).
    // LV_IMG_BUF_SIZE_TRUE_COLOR_ALPHA already encodes 3 bytes per pixel for this depth.
    uint8_t *dst = sAvatarBuffer;
    for (size_t i = 0; i < (size_t)width * (size_t)height; i++)
    {
        const uint8_t r = pImgData[(i * 4U) + 0];
        const uint8_t g = pImgData[(i * 4U) + 1];
        const uint8_t b = pImgData[(i * 4U) + 2];
        const uint8_t a = pImgData[(i * 4U) + 3];

        const uint16_t rgb565 = (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3));
        *dst++ = (uint8_t)(rgb565 & 0xFF);
        *dst++ = (uint8_t)((rgb565 >> 8) & 0xFF);
        *dst++ = a;
    }
#else
    // LV_COLOR_DEPTH == 32: data already matches LVGL layout (ARGB8888). Copy as-is.
    memcpy(sAvatarBuffer, pImgData, required);
#endif

    sAvatarDsc.header.always_zero = 0;
    sAvatarDsc.header.w = width;
    sAvatarDsc.header.h = height;
    sAvatarDsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    sAvatarDsc.data_size = required;
    sAvatarDsc.data = sAvatarBuffer;

    const AchievementUserProfile_t profile = {
        .pUsername = pUsername,
        .Points = points,
        .pAvatar = &sAvatarDsc,
        .AvatarScalePct = avatarScalePct,
    };

    return Achievement_SetUserProfile(&profile);
}
