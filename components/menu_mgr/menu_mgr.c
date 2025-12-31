#include "menu_mgr.h"

#include "osd_shared.h"
#include "lvgl.h"
#include "esp_log.h"
#include "button.h"
#include "system/cheats.h"

#include <stddef.h>
#include <stdint.h>

typedef struct MenuMgrCtx
{
    TabID_t eCurTab;
    MenuTab_t* pMenus[kNumTabIDs];
} MenuMgrCtx_t;

const lv_point_t _MenuOrigin_px = {
    .x = 8,
    .y = 7,
};

static MenuMgrCtx_t _Ctx;
static const char* TAG = "MenuMgr";
static uint32_t _LastTabNavMs;
static lv_obj_t* _DbgLabel;
static lv_obj_t* _pScreen;
static bool _DbgEnabled;
static OSD_Result_t MenuMgr_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
static OSD_Result_t MenuMgr_Draw(void* arg);
static OSD_Result_t MenuMgr_OnTransition(void *arg);
static void MenuMgr_NextTab(void);
static void MenuMgr_PrevTab(void);
static bool MenuMgr_TabNavAllowed(void);
static void MenuMgr_DebugLabelUpdate(TabID_t eID, Button_t Button, ButtonState_t State);

OSD_Result_t MenuMgr_Initialize(OSD_Widget_t* const pWidget, lv_obj_t *const pScreen)
{
    if (pScreen == NULL)
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    if (sys_dnode_is_linked(&pWidget->Node))
    {
        return kOSD_Result_Err_MenuAlreadyInit;
    }

    pWidget->fnDraw = MenuMgr_Draw;
    pWidget->fnOnButton = MenuMgr_OnButton;
    pWidget->fnOnTransition = MenuMgr_OnTransition;

    _Ctx.eCurTab = kTabID_First;
    _pScreen = pScreen;
    _DbgEnabled = false; // set true for on-screen tab/button debugging

    return kOSD_Result_Ok;
}

OSD_Result_t MenuMgr_AddTab(TabID_t eID, MenuTab_t *const pTab)
{
    if ((unsigned)eID >= kNumTabIDs)
    {
        return kOSD_Result_Err_InvalidTabID;
    }

    if ((pTab == NULL) || (pTab->pImageDesc == NULL))
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    if ((_Ctx.pMenus[eID] != NULL) || (pTab->pImgObj != NULL))
    {
        return kOSD_Result_Err_MenuTabAlreadyInit;
    }

    _Ctx.pMenus[eID] = pTab;

    return kOSD_Result_Ok;
}

static OSD_Result_t MenuMgr_Draw(void* arg)
{
    if (arg == NULL)
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    lv_obj_t *const pScreen = (lv_obj_t*)arg;
    _pScreen = pScreen;

    const TabID_t eID = _Ctx.eCurTab;

    if ((unsigned)eID >= kNumTabIDs)
    {
        return kOSD_Result_Err_InvalidTabID;
    }

    MenuTab_t *const pTab = _Ctx.pMenus[eID];
    if ((pTab == NULL) || (pTab->pImageDesc == NULL))
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    if (pTab->pImgObj == NULL)
    {
        pTab->pImgObj = lv_img_create(pScreen);
        lv_obj_align(pTab->pImgObj , LV_ALIGN_TOP_LEFT, _MenuOrigin_px.x, _MenuOrigin_px.y);
    }

    lv_img_set_src(pTab->pImgObj, pTab->pImageDesc);

    if (pTab->Widget.fnDraw != NULL)
    {
        Tab_DrawCtx_t Ctx = { pTab->Menu, pScreen, pTab->Accent };
        pTab->Widget.fnDraw(&Ctx);
    }

    return kOSD_Result_Ok;
}

static OSD_Result_t MenuMgr_OnButton(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;

    const TabID_t eID = _Ctx.eCurTab;

    if ((unsigned)eID >= kNumTabIDs)
    {
        return kOSD_Result_Err_InvalidTabID;
    }

    MenuTab_t *const pTab = _Ctx.pMenus[eID];
    if (pTab == NULL)
    {
        ESP_LOGE(TAG, "no tab in %s", __func__ );
        return kOSD_Result_Err_NullDataPtr;
    }


    switch (Button)
    {
        case kButton_Start:
        case kButton_Select:
        case kButton_B:
        case kButton_A:
        case kButton_Down:
        case kButton_Up:
            if ((eID == kTabID_Cheats) && Cheats_IsInEdit() &&
                (pTab->Menu != NULL) && (pTab->Menu->pCurrent != NULL) &&
                (pTab->Menu->pCurrent->Widget.fnOnButton != NULL))
            {
                const OSD_Result_t eResult = pTab->Menu->pCurrent->Widget.fnOnButton(Button, State, pTab->Menu);

                if (eResult != kOSD_Result_Ok)
                {
                    ESP_LOGE(TAG, "%s OnButton call failed with %d", pTab->Menu->pCurrent->Widget.Name, eResult);
                }
            }
            else if (pTab->Widget.fnOnButton != NULL)
            {
                const OSD_Result_t eResult = pTab->Widget.fnOnButton(Button, State, pTab->Menu);

                if (eResult != kOSD_Result_Ok)
                {
                    ESP_LOGE(TAG, "%s OnButton call failed with %d", pTab->Widget.Name, eResult);
                }
            }
            break;

        case kButton_Left:
            if ((eID == kTabID_Cheats) && Cheats_IsInEdit())
            {
                if ((pTab->Menu != NULL) && (pTab->Menu->pCurrent != NULL) &&
                    (pTab->Menu->pCurrent->Widget.fnOnButton != NULL))
                {
                    return pTab->Menu->pCurrent->Widget.fnOnButton(Button, State, pTab->Menu);
                }
                return kOSD_Result_Ok;
            }

            if ((State == kButtonState_Pressed) && MenuMgr_TabNavAllowed())
            {
                if (eID == kTabID_Cheats)
                {
                    // Explicitly step to System when leaving Cheats (transition skipped to avoid OSD reset)
                    _Ctx.eCurTab = kTabID_System;
                    _LastTabNavMs = lv_tick_get();
                }
                else
                {
                    MenuMgr_PrevTab();
                }
            }
            break;

        case kButton_Right:
            if ((eID == kTabID_Cheats) && Cheats_IsInEdit())
            {
                if ((pTab->Menu != NULL) && (pTab->Menu->pCurrent != NULL) &&
                    (pTab->Menu->pCurrent->Widget.fnOnButton != NULL))
                {
                    return pTab->Menu->pCurrent->Widget.fnOnButton(Button, State, pTab->Menu);
                }
                return kOSD_Result_Ok;
            }

            if ((State == kButtonState_Pressed) && MenuMgr_TabNavAllowed())
            {
                if (eID == kTabID_Cheats)
                {
                    // Explicitly step to Achievement when leaving Cheats (transition skipped to avoid OSD reset)
                    _Ctx.eCurTab = kTabID_Achievement;
                    _LastTabNavMs = lv_tick_get();
                }
                else
                {
                    MenuMgr_NextTab();
                }
            }
            break;

        case kButton_MenuEn:
        case kButton_MenuEnAlt:
        case kButton_None:
        case kNumButtons:
            // No action required; included to silence -Wswitch-enum
            break;
    }

    MenuMgr_DebugLabelUpdate(_Ctx.eCurTab, Button, State);
    return kOSD_Result_Ok;
}

static void MenuMgr_DebugLabelUpdate(TabID_t eID, Button_t Button, ButtonState_t State)
{
    if (!_DbgEnabled || (_pScreen == NULL))
    {
        return;
    }

    if (_DbgLabel == NULL)
    {
        _DbgLabel = lv_label_create(_pScreen);
        lv_obj_set_pos(_DbgLabel, 4, 2);
        lv_obj_add_style(_DbgLabel, OSD_GetStyleTextGrey(), 0);
    }

    char buf[64];
    lv_snprintf(buf, sizeof(buf), "tab=%d btn=%s state=%d edit=%d", (int)eID, Button_GetNameStr(Button), (int)State, Cheats_IsInEdit());
    lv_label_set_text(_DbgLabel, buf);
    lv_obj_move_foreground(_DbgLabel);
}

static void MenuMgr_NextTab(void)
{
    TabID_t eNextID = _Ctx.eCurTab;
    for (size_t i = 0; i < kNumTabIDs; ++i)
    {
        eNextID = (eNextID + 1) % kNumTabIDs;
        if (_Ctx.pMenus[eNextID] != NULL)
        {
            // Clean up the old tab data
            MenuMgr_OnTransition(NULL);
            _Ctx.eCurTab = eNextID;
            _LastTabNavMs = lv_tick_get();
            return;
        }
    }
}

static void MenuMgr_PrevTab(void)
{
    TabID_t ePrevID = _Ctx.eCurTab;
    for (size_t i = 0; i < kNumTabIDs; ++i)
    {
        ePrevID = (ePrevID == 0) ? kTabID_Last : (ePrevID - 1);
        if (_Ctx.pMenus[ePrevID] != NULL)
        {
            // Clean up the old tab data
            MenuMgr_OnTransition(NULL);
            _Ctx.eCurTab = ePrevID;
            _LastTabNavMs = lv_tick_get();
            return;
        }
    }
}

static bool MenuMgr_TabNavAllowed(void)
{
    const uint32_t elapsed = lv_tick_elaps(_LastTabNavMs);
    return (elapsed > 180u); // debounce accidental double-step
}

static OSD_Result_t MenuMgr_OnTransition(void *arg)
{
    (void) arg;
    OSD_Result_t eResult = kOSD_Result_Ok;

    MenuTab_t *const pTab = _Ctx.pMenus[_Ctx.eCurTab];

    if (pTab->Widget.fnOnTransition != NULL)
    {
        eResult = pTab->Widget.fnOnTransition(pTab->Menu);
        if (eResult != kOSD_Result_Ok)
        {
            ESP_LOGE(TAG, "%s OnTransition failed with %d", pTab->Widget.Name, eResult);
        }
    }

    if (pTab->pImgObj != NULL)
    {
        lv_obj_del(pTab->pImgObj);
        pTab->pImgObj = NULL;
    }

    return eResult;
}
