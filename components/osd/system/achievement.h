#pragma once

#include "osd_shared.h"
#include "lvgl.h"

#include <stddef.h>

typedef struct AchievementUserProfile {
	const char *pUsername;
	uint32_t Points;
	const lv_img_dsc_t *pAvatar;
	// 100 == native size, 50 == half-size. Clamped to [10, 100].
	uint8_t AvatarScalePct;
} AchievementUserProfile_t;

OSD_Result_t Achievement_Draw(void *arg);
OSD_Result_t Achievement_OnTransition(void *arg);
OSD_Result_t Achievement_OnButton(const Button_t button, const ButtonState_t state, void *arg);
// Allow the phone → Chromatic pipeline to push fresh profile data.
OSD_Result_t Achievement_SetUserProfile(const AchievementUserProfile_t *pProfile);
// Returns a persistent RGBA scratch buffer (up to 128x128) for avatar staging; capacity is reported in bytes.
OSD_Result_t Achievement_GetAvatarScratch(uint8_t **ppBuf, size_t *pCapacity);
// Convenience helper to push a raw RGBA avatar; width/height up to 128px.
OSD_Result_t Achievement_UpdateFromRawImage(const char *pUsername,
											uint32_t points,
											const uint8_t *pImgData,
											size_t imgDataLen,
											uint16_t width,
											uint16_t height,
											uint8_t avatarScalePct);
