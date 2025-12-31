#pragma once

#include "lvgl.h"

void Gfx_Start(lv_obj_t *const pScreen);

// Toggle visibility of the OSD root container; returns previous visibility state.
bool Gfx_SetOSDVisible(bool visible);
