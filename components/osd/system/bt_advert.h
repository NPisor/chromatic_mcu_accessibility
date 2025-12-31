#pragma once

#include "osd_shared.h"

OSD_Result_t BTAdvert_Draw(void *arg);
OSD_Result_t BTAdvert_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
OSD_Result_t BTAdvert_OnTransition(void *arg);
