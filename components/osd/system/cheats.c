#include "cheats.h"

#include "fpga_tx.h"
#include "osd_shared.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "lvgl.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    kCheat_SlotsMax   = 8,
    kCheat_CodeChars  = 8,

    // Right pane layout
    kPanelX_px        = 74,
    kPanelY_px        = 42,
    kPanelW_px        = 128,
    kPanelH_px        = 64,
    kLineH_px         = 8,

    // Left pane background bounds
    kLeftPanelX_px    = 15,
    kLeftPanelY_px    = 42,
    kLeftPanelW_px    = 72,
    kLeftPanelH_px    = 60,

    // Left-pane status hints
    kStatusX_px       = 15,
    kStatusY_px       = 52,
    kStatusW_px       = 60,
    kStatusH_px       = 60,
};

typedef struct CheatSlot {
    char Code[kCheat_CodeChars + 1];
    uint8_t Type;
    uint8_t Value;
    uint16_t Addr;
    bool Enabled;
    bool Valid;
} CheatSlot_t;

typedef struct Cheats {
    CheatSlot_t Slots[kCheat_SlotsMax];
    uint8_t ActiveIdx;
    uint8_t SlotCount;
    int8_t CursorIdx; // -1 targets the Slot label, >=0 targets a digit
    bool EditMode;
    bool LastEnabled[kCheat_SlotsMax];
    lv_obj_t* pCodeObjs[kCheat_SlotsMax];
    lv_obj_t* pPanelObj;
    lv_obj_t* pLeftPanelObj;
    lv_obj_t* pHelpObj;
    lv_obj_t* pCursorObj;
    lv_obj_t* pEditModeObj;

} Cheats_t;

static Cheats_t _Ctx;
static const char* TAG = "cheats";
static bool _PersistLoaded;
static bool _ReloadOnNextDraw;
static char _GameKey[17] = "default"; // NVS key (ASCII, null-terminated)
static bool _Dirty;

typedef struct __attribute__((packed)) PersistCheats {
    uint8_t Version;
    uint8_t SlotCount;
    uint8_t ActiveIdx;
    uint8_t Reserved;
    CheatSlot_t Slots[kCheat_SlotsMax];
} PersistCheats_t;

static void UpdateLabels(void);
static void SetEditMode(const bool enable);
static void ResetSlot(CheatSlot_t* pSlot);
static void ShiftSlotsDown(uint8_t fromIdx);
static void ShiftSlotsUp(uint8_t fromIdx);
static bool IsValidCode(const char* code);
static bool ParseCheat(CheatSlot_t* pSlot);
static bool CodesEqual(const char* a, const char* b);
static int FindSlotIndex(const char* code);
static bool NormalizeLineToCode(const char* line, size_t len, char out[9], bool* isRemove);
static bool IsPlaceholderSlot(const CheatSlot_t* slot);
static uint8_t EffectiveSlotCount(void);
static void PushSlotToFPGA(uint8_t idx, const CheatSlot_t* slot);
static void PushSlotsToFPGA(uint8_t prevCount);
static uint8_t NibbleFromChar(char c);
static char CharFromNibble(uint8_t n);
static void SendActiveToFPGA(void);
static void TraceStatus(const char* action, const CheatSlot_t* pSlot);
static void Cheats_LoadPersist(void);
static void Cheats_SavePersist(void);
static const char* Cheats_GetKey(void);
static void Cheats_SetKeyFromHash(uint32_t h);
static uint32_t Cheats_HashTitle16(const uint8_t title16[16]);
static bool IsHexChar(char c);
static void UpdateHelpText(void);

OSD_Result_t Cheats_Draw(void* arg)
{
    if (arg == NULL)
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    if (_ReloadOnNextDraw)
    {
        Cheats_LoadPersist();
        _ReloadOnNextDraw = false;
    }

    lv_obj_t *const pScreen = (lv_obj_t *const) arg;

    // Set the screen background to opaque black so any transparent pixels in the tab art
    // render as black instead of the magenta key color. Avoid resizing the root.
    lv_obj_set_style_bg_color(pScreen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pScreen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_opa(pScreen, LV_OPA_TRANSP, LV_PART_MAIN);

    // Left pane background to ensure we don't expose the transparent key color
    if (_Ctx.pLeftPanelObj == NULL)
    {
        _Ctx.pLeftPanelObj = lv_obj_create(pScreen);
        lv_obj_set_pos(_Ctx.pLeftPanelObj, kLeftPanelX_px, kLeftPanelY_px);
        lv_obj_set_size(_Ctx.pLeftPanelObj, kLeftPanelW_px, kLeftPanelH_px + 12);
        lv_obj_set_scrollbar_mode(_Ctx.pLeftPanelObj, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_color(_Ctx.pLeftPanelObj, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_Ctx.pLeftPanelObj, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_opa(_Ctx.pLeftPanelObj, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_pad_all(_Ctx.pLeftPanelObj, 0, LV_PART_MAIN);
        lv_obj_move_background(_Ctx.pLeftPanelObj); // keep tab dots and arrows visible above the overlay
    }

    // Help text on the left side
    if (_Ctx.pHelpObj == NULL)
    {
        _Ctx.pHelpObj = lv_label_create(pScreen);
        lv_obj_add_style(_Ctx.pHelpObj, OSD_GetStyleTextGrey(), 0);
        lv_obj_set_pos(_Ctx.pHelpObj, kLeftPanelX_px, kLeftPanelY_px + 12);
        lv_obj_set_size(_Ctx.pHelpObj, kLeftPanelW_px, kLeftPanelH_px);
        lv_label_set_long_mode(_Ctx.pHelpObj, LV_LABEL_LONG_WRAP);
        UpdateHelpText();
    }

    // Panel container to bound the slot labels
    if (_Ctx.pPanelObj == NULL)
    {
        _Ctx.pPanelObj = lv_obj_create(pScreen);
        lv_obj_set_pos(_Ctx.pPanelObj, kPanelX_px, kPanelY_px);
        lv_obj_set_size(_Ctx.pPanelObj, kPanelW_px, kPanelH_px);
        lv_obj_set_scrollbar_mode(_Ctx.pPanelObj, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_opa(_Ctx.pPanelObj, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_opa(_Ctx.pPanelObj, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_pad_all(_Ctx.pPanelObj, 0, LV_PART_MAIN);
    }

    // Create per-slot labels
    for (uint8_t i = 0; i < kCheat_SlotsMax; i++)
    {
        if (_Ctx.pCodeObjs[i] == NULL)
        {
            _Ctx.pCodeObjs[i] = lv_label_create(_Ctx.pPanelObj);
            lv_obj_add_style(_Ctx.pCodeObjs[i], OSD_GetStyleTextWhite(), 0);
            lv_obj_set_pos(_Ctx.pCodeObjs[i], 0, i * kLineH_px);
            lv_obj_set_width(_Ctx.pCodeObjs[i], kPanelW_px);
            lv_label_set_long_mode(_Ctx.pCodeObjs[i], LV_LABEL_LONG_WRAP);
        }
    }

    // Caret label
    if (_Ctx.pCursorObj == NULL)
    {
        _Ctx.pCursorObj = lv_label_create(pScreen);
        lv_obj_add_style(_Ctx.pCursorObj, OSD_GetStyleTextWhite(), 0);
        lv_obj_set_style_bg_opa(_Ctx.pCursorObj, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_all(_Ctx.pCursorObj, 1, LV_PART_MAIN);
    }

    // Edit mode indicator
    if (_Ctx.pEditModeObj == NULL)
    {
        _Ctx.pEditModeObj = lv_label_create(pScreen);
        lv_obj_add_style(_Ctx.pEditModeObj, OSD_GetStyleTextWhite(), 0);
        lv_obj_set_style_text_color(_Ctx.pEditModeObj, lv_color_hex(0x55FF55), LV_PART_MAIN);
        lv_label_set_text(_Ctx.pEditModeObj, "EDIT MODE");
        lv_obj_set_pos(_Ctx.pEditModeObj, kLeftPanelX_px + 4, kLeftPanelY_px + kLeftPanelH_px);
        lv_obj_add_flag(_Ctx.pEditModeObj, LV_OBJ_FLAG_HIDDEN);
    }

    // Ensure at least one placeholder slot exists even before entering edit mode
    if (_Ctx.SlotCount == 0)
    {
        _Ctx.SlotCount = 1;
        _Ctx.ActiveIdx = 0;
        ResetSlot(&_Ctx.Slots[0]);
    }

    UpdateLabels();

    return kOSD_Result_Ok;
}

OSD_Result_t Cheats_OnButton(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;

    if (State != kButtonState_Pressed)
    {
        return kOSD_Result_Ok;
    }

    const uint8_t effCount = EffectiveSlotCount();
    CheatSlot_t* pSlot = (effCount > 0 && _Ctx.ActiveIdx < kCheat_SlotsMax) ? &_Ctx.Slots[_Ctx.ActiveIdx] : NULL;

    switch (Button)
    {
        case kButton_Up:
            if (effCount > 0)
            {
                if (_Ctx.EditMode && (_Ctx.CursorIdx <= -1))
                {
                    // Move between slots when the label is selected
                    _Ctx.ActiveIdx = (_Ctx.ActiveIdx == 0) ? (effCount - 1) : (_Ctx.ActiveIdx - 1);
                }
                else if (_Ctx.EditMode && (_Ctx.CursorIdx >= 0))
                {
                    // Increment current nibble
                    CheatSlot_t *slot = &_Ctx.Slots[_Ctx.ActiveIdx];
                    if (slot->Enabled)
                    {
                        slot->Enabled = false;
                        FPGA_Tx_SendCheatParsed(_Ctx.ActiveIdx, false, slot->Type, slot->Addr, slot->Value);
                        _Ctx.LastEnabled[_Ctx.ActiveIdx] = false;
                    }
                    char *code = slot->Code;
                    uint8_t nib = NibbleFromChar(code[_Ctx.CursorIdx]);
                    nib = (uint8_t)((nib + 1u) & 0xFu);
                    code[_Ctx.CursorIdx] = CharFromNibble(nib);
                    slot->Valid = ParseCheat(slot);
                    _Dirty = true;
                    SendActiveToFPGA();
                }
                else
                {
                    _Ctx.ActiveIdx = (_Ctx.ActiveIdx == 0) ? (effCount - 1) : (_Ctx.ActiveIdx - 1);
                }
            }
            break;
        case kButton_Down:
            if (effCount > 0)
            {
                if (_Ctx.EditMode && (_Ctx.CursorIdx <= -1))
                {
                    _Ctx.ActiveIdx = (_Ctx.ActiveIdx + 1) % effCount;
                }
                else if (_Ctx.EditMode && (_Ctx.CursorIdx >= 0))
                {
                    // Decrement current nibble
                    CheatSlot_t *slot = &_Ctx.Slots[_Ctx.ActiveIdx];
                    if (slot->Enabled)
                    {
                        slot->Enabled = false;
                        FPGA_Tx_SendCheatParsed(_Ctx.ActiveIdx, false, slot->Type, slot->Addr, slot->Value);
                        _Ctx.LastEnabled[_Ctx.ActiveIdx] = false;
                    }
                    char *code = slot->Code;
                    uint8_t nib = NibbleFromChar(code[_Ctx.CursorIdx]);
                    nib = (uint8_t)((nib + 15u) & 0xFu);
                    code[_Ctx.CursorIdx] = CharFromNibble(nib);
                    slot->Valid = ParseCheat(slot);
                    _Dirty = true;
                    SendActiveToFPGA();
                }
                else
                {
                    _Ctx.ActiveIdx = (_Ctx.ActiveIdx + 1) % effCount;
                }
            }
            break;
        case kButton_Left:
            if (_Ctx.EditMode && effCount > 0)
            {
                if (_Ctx.CursorIdx <= -1)
                {
                    _Ctx.CursorIdx = kCheat_CodeChars - 1;
                }
                else if (_Ctx.CursorIdx == 0)
                {
                    _Ctx.CursorIdx = -1; // jump to label
                }
                else
                {
                    _Ctx.CursorIdx--;
                }
            }
            break;
        case kButton_Right:
            if (_Ctx.EditMode && effCount > 0)
            {
                if (_Ctx.CursorIdx < 0)
                {
                    _Ctx.CursorIdx = 0;
                }
                else if (_Ctx.CursorIdx >= (kCheat_CodeChars - 1))
                {
                    _Ctx.CursorIdx = -1; // wrap to label
                }
                else
                {
                    _Ctx.CursorIdx++;
                }
            }
            break;
        case kButton_A:
            SetEditMode(!_Ctx.EditMode);
            break;
        case kButton_Start:
            if (_Ctx.EditMode)
            {
                if (_Ctx.SlotCount < kCheat_SlotsMax)
                {
                    const uint8_t prevCount = EffectiveSlotCount();
                    CheatSlot_t *slot = &_Ctx.Slots[_Ctx.SlotCount];
                    ResetSlot(slot);
                    slot->Enabled = false;
                    _Ctx.ActiveIdx = _Ctx.SlotCount;
                    _Ctx.SlotCount++;
                    _Ctx.CursorIdx = 0;
                    _Dirty = true;
                    PushSlotsToFPGA(prevCount);
                }
                else
                {
                    // Max slots reached; no status text shown
                }
            }
            else
            {
                // Not in edit mode; no status text shown
            }
            break;
        case kButton_Select:
            if (_Ctx.EditMode)
            {
                const uint8_t prevCount = EffectiveSlotCount();
                if (prevCount > 0 && _Ctx.ActiveIdx < _Ctx.SlotCount)
                {
                    if (_Ctx.SlotCount == 1)
                    {
                        if (!IsPlaceholderSlot(&_Ctx.Slots[0]))
                        {
                            ResetSlot(&_Ctx.Slots[0]);
                            _Ctx.ActiveIdx = 0;
                            _Ctx.CursorIdx = 0;
                            _Dirty = true;
                            PushSlotsToFPGA(prevCount);
                        }
                        else
                        {
                            // Placeholder untouched; nothing to remove
                        }
                    }
                    else
                    {
                        ShiftSlotsUp(_Ctx.ActiveIdx);
                        _Ctx.SlotCount--;
                        ResetSlot(&_Ctx.Slots[_Ctx.SlotCount]);
                        _Ctx.LastEnabled[_Ctx.SlotCount] = false;
                        if (_Ctx.ActiveIdx >= _Ctx.SlotCount)
                        {
                            _Ctx.ActiveIdx = _Ctx.SlotCount - 1;
                        }
                        _Ctx.CursorIdx = (_Ctx.CursorIdx < -1) ? -1 : _Ctx.CursorIdx;
                        _Dirty = true;
                        PushSlotsToFPGA(prevCount);
                    }
                }
                else
                {
                    // Nothing to remove
                }
            }
            else
            {
                // Not in edit mode; no status text shown
            }
            break;
        case kButton_B:
            if ((effCount > 0) && (pSlot != NULL) && pSlot->Valid)
            {
                pSlot->Enabled = !pSlot->Enabled;
                ESP_LOGI(TAG, "toggle slot=%u enabled=%u type=0x%02X addr=0x%04X val=0x%02X", (unsigned)_Ctx.ActiveIdx, (unsigned)pSlot->Enabled, (unsigned)pSlot->Type, (unsigned)pSlot->Addr, (unsigned)pSlot->Value);
                PushSlotsToFPGA(effCount); // push all slots to ensure disable takes effect
                _Dirty = true;
                Cheats_SavePersist();
                TraceStatus(pSlot->Enabled ? "toggle_on" : "toggle_off", pSlot);
            }
            else
            {
                // Invalid or no slot; no status text shown
            }
            break;
        default:
            break;
    }

    UpdateLabels();
    return kOSD_Result_Ok;
}

OSD_Result_t Cheats_OnTransition(void* arg)
{
    (void)arg;

    lv_obj_t* toDelete[] = {
        _Ctx.pPanelObj,
        _Ctx.pLeftPanelObj,
        _Ctx.pCursorObj,
        _Ctx.pHelpObj,
        _Ctx.pEditModeObj,
    };

    for (size_t i = 0; i < sizeof(toDelete)/sizeof(toDelete[0]); i++)
    {
        if (toDelete[i] != NULL)
        {
            lv_obj_del(toDelete[i]);
        }
    }

    _Ctx.pPanelObj = NULL;
    _Ctx.pLeftPanelObj = NULL;
    _Ctx.pCursorObj = NULL;
    _Ctx.pHelpObj = NULL;
    _Ctx.pEditModeObj = NULL;

    for (size_t i = 0; i < kCheat_SlotsMax; i++)
    {
        if (_Ctx.pCodeObjs[i] != NULL)
        {
            lv_obj_del(_Ctx.pCodeObjs[i]);
            _Ctx.pCodeObjs[i] = NULL;
        }
    }

    Cheats_SavePersist();
    _ReloadOnNextDraw = true;

    return kOSD_Result_Ok;
}

void Cheats_Reset(void)
{
    memset(&_Ctx, 0x0, sizeof(_Ctx));
    for (size_t i = 0; i < kCheat_SlotsMax; i++)
    {
        ResetSlot(&_Ctx.Slots[i]);
        _Ctx.LastEnabled[i] = false;
    }

    _Ctx.EditMode = false;
    _Ctx.SlotCount = 0;
    _Ctx.ActiveIdx = 0;
    _Ctx.CursorIdx = 0;
}

bool Cheats_IsInEdit(void)
{
    return _Ctx.EditMode;
}

bool Cheats_HasEnabled(void)
{
    for (uint8_t i = 0; i < _Ctx.SlotCount; i++)
    {
        if (_Ctx.Slots[i].Enabled)
        {
            return true;
        }
    }
    return false;
}

static void UpdateLabels(void)
{
    const uint8_t effCount = EffectiveSlotCount();

    if (effCount == 0)
    {
        _Ctx.ActiveIdx = 0;
    }
    else if (_Ctx.ActiveIdx >= effCount)
    {
        _Ctx.ActiveIdx = effCount - 1;
    }

    for (uint8_t i = 0; i < kCheat_SlotsMax; i++)
    {
        lv_obj_t* pLbl = _Ctx.pCodeObjs[i];
        if (pLbl == NULL)
        {
            continue;
        }

        if (i < effCount)
        {
            CheatSlot_t* pS = &_Ctx.Slots[i];
            lv_label_set_text_fmt(pLbl, "Slot %u: %s%s", (unsigned)i + 1,
                                  pS->Code,
                                  pS->Enabled ? " (ON)" : " (OFF)");
            lv_obj_clear_flag(pLbl, LV_OBJ_FLAG_HIDDEN);

            if (_Ctx.EditMode && (i == _Ctx.ActiveIdx))
            {
                lv_color_t col = (_Ctx.CursorIdx <= -1) ? lv_color_hex(0xFFFF66) : lv_color_hex(0x55FF55);
                lv_obj_set_style_text_color(pLbl, col, LV_PART_MAIN);
            }
            else if (i == _Ctx.ActiveIdx)
            {
                lv_obj_set_style_text_color(pLbl, lv_color_white(), LV_PART_MAIN);
            }
            else
            {
                lv_obj_set_style_text_color(pLbl, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
            }
        }
        else
        {
            lv_label_set_text(pLbl, "");
            lv_obj_add_flag(pLbl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (_Ctx.pCursorObj != NULL)
    {
        if (effCount == 0)
        {
            lv_label_set_text_static(_Ctx.pCursorObj, "");
            lv_obj_add_flag(_Ctx.pCursorObj, LV_OBJ_FLAG_HIDDEN);
            return;
        }

        lv_obj_t* pActiveLbl = (_Ctx.ActiveIdx < kCheat_SlotsMax) ? _Ctx.pCodeObjs[_Ctx.ActiveIdx] : NULL;
        const lv_font_t* font = pActiveLbl ? lv_obj_get_style_text_font(pActiveLbl, LV_PART_MAIN) : NULL;
        if (font == NULL && pActiveLbl != NULL) { font = lv_theme_get_font_small(pActiveLbl); }
        if (font == NULL)
        {
            font = lv_theme_get_font_small(NULL);
        }
        const lv_coord_t char_w = lv_font_get_glyph_width(font, '0', 0);

        char prefix[16];
        lv_snprintf(prefix, sizeof(prefix), "Slot %u: ", (unsigned)_Ctx.ActiveIdx + 1);
        lv_point_t sz = {0};
        lv_txt_get_size(&sz, prefix, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

        if (_Ctx.EditMode && (_Ctx.CursorIdx >= 0))
        {
            const char *code = _Ctx.Slots[_Ctx.ActiveIdx].Code;
            const char ch = (_Ctx.CursorIdx < kCheat_CodeChars) ? code[_Ctx.CursorIdx] : ' ';
            lv_label_set_text_fmt(_Ctx.pCursorObj, "%c", ch);
            lv_obj_set_style_bg_color(_Ctx.pCursorObj, lv_color_white(), LV_PART_MAIN);
            lv_obj_set_style_text_color(_Ctx.pCursorObj, lv_color_black(), LV_PART_MAIN);

            lv_coord_t caret_x = kPanelX_px + sz.x + (lv_coord_t)(_Ctx.CursorIdx * char_w);
            lv_obj_set_pos(_Ctx.pCursorObj, caret_x, kPanelY_px + (_Ctx.ActiveIdx * kLineH_px) + 1);
            lv_obj_set_size(_Ctx.pCursorObj, char_w + 2, kLineH_px);
            lv_obj_clear_flag(_Ctx.pCursorObj, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_label_set_text_static(_Ctx.pCursorObj, "");
            lv_obj_add_flag(_Ctx.pCursorObj, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (_Ctx.pEditModeObj != NULL)
    {
        if (_Ctx.EditMode)
        {
            lv_obj_clear_flag(_Ctx.pEditModeObj, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(_Ctx.pEditModeObj, LV_OBJ_FLAG_HIDDEN);
        }
    }

    UpdateHelpText();
}

static void UpdateHelpText(void)
{
    if (_Ctx.pHelpObj == NULL)
    {
        return;
    }

    if (_Ctx.EditMode)
    {
        lv_label_set_text(_Ctx.pHelpObj,
            "A: Edit ON/OFF\n"
            "U/D: Edit Value\n"
            "L/R: Cursor Pos\n"
            "Start: Add\n"
            "Select: Remove");
    }
    else
    {
        lv_label_set_text(_Ctx.pHelpObj,
            "A: Edit ON/OFF\n"
            "B: Toggle ON/OFF\n"
            "U/D: Select");
    }
}

static void SetEditMode(const bool enable)
{
    const bool was_edit = _Ctx.EditMode;
    _Ctx.EditMode = enable;

    if (_Ctx.EditMode && (_Ctx.SlotCount == 0))
    {
        _Ctx.SlotCount = 1;
        _Ctx.ActiveIdx = 0;
        ResetSlot(&_Ctx.Slots[0]);
    }

    if (_Ctx.EditMode)
    {
        _Ctx.CursorIdx = (_Ctx.CursorIdx < -1) ? -1 : _Ctx.CursorIdx;
    }
    else
    {
        if (was_edit && _Dirty)
        {
            Cheats_SavePersist();
        }
    }

    UpdateHelpText();
}

static void ResetSlot(CheatSlot_t* pSlot)
{
    if (pSlot == NULL)
    {
        return;
    }

    memcpy(pSlot->Code, "01000000", kCheat_CodeChars);
    pSlot->Code[kCheat_CodeChars] = '\0';
    pSlot->Enabled = false;
    pSlot->Valid = ParseCheat(pSlot);
}

static bool CodesEqual(const char* a, const char* b)
{
    if ((a == NULL) || (b == NULL))
    {
        return false;
    }

    for (size_t i = 0; i < kCheat_CodeChars; i++)
    {
        const char ca = (char)toupper((unsigned char)a[i]);
        const char cb = (char)toupper((unsigned char)b[i]);
        if (ca != cb)
        {
            return false;
        }
    }
    return true;
}

static int FindSlotIndex(const char* code)
{
    if (code == NULL)
    {
        return -1;
    }
    for (uint8_t i = 0; i < _Ctx.SlotCount; i++)
    {
        if (CodesEqual(_Ctx.Slots[i].Code, code))
        {
            return (int)i;
        }
    }
    return -1;
}

static void ShiftSlotsDown(uint8_t fromIdx)
{
    if (fromIdx >= kCheat_SlotsMax)
    {
        return;
    }

    for (int i = kCheat_SlotsMax - 1; i > (int)fromIdx; i--)
    {
        _Ctx.Slots[i] = _Ctx.Slots[i - 1];
        _Ctx.LastEnabled[i] = _Ctx.LastEnabled[i - 1];
    }
}

static void ShiftSlotsUp(uint8_t fromIdx)
{
    if (fromIdx >= kCheat_SlotsMax)
    {
        return;
    }

    for (uint8_t i = fromIdx; i + 1 < kCheat_SlotsMax; i++)
    {
        _Ctx.Slots[i] = _Ctx.Slots[i + 1];
        _Ctx.LastEnabled[i] = _Ctx.LastEnabled[i + 1];
    }
}

static bool NormalizeLineToCode(const char* line, size_t len, char out[9], bool* isRemove)
{
    if ((line == NULL) || (out == NULL))
    {
        return false;
    }

    // Trim leading whitespace
    while ((len > 0) && isspace((unsigned char)*line))
    {
        line++;
        len--;
    }

    bool removeFlag = false;
    if ((len > 0) && (*line == '-'))
    {
        removeFlag = true;
        line++;
        len--;
        while ((len > 0) && isspace((unsigned char)*line))
        {
            line++;
            len--;
        }
    }

    // Trim trailing whitespace/newlines
    while ((len > 0) && isspace((unsigned char)line[len - 1]))
    {
        len--;
    }

    if (len < kCheat_CodeChars)
    {
        return false;
    }

    for (size_t i = 0; i < kCheat_CodeChars; i++)
    {
        char c = line[i];
        if (!isxdigit((unsigned char)c))
        {
            return false;
        }
        out[i] = (char)toupper((unsigned char)c);
    }
    out[kCheat_CodeChars] = '\0';

    if (isRemove != NULL)
    {
        *isRemove = removeFlag;
    }
    return true;
}

static bool IsValidCode(const char* code)
{
    if (code == NULL)
    {
        return false;
    }

    // Require exactly 8 hexadecimal characters and a null terminator
    for (size_t i = 0; i < kCheat_CodeChars; i++)
    {
        const char c = code[i];
        if (c == '\0' || !isxdigit((unsigned char)c))
        {
            return false;
        }
    }

    return code[kCheat_CodeChars] == '\0';
}

static bool ParseCheat(CheatSlot_t* pSlot)
{
    if ((pSlot == NULL) || !IsValidCode(pSlot->Code))
    {
        return false;
    }

    const uint8_t type  = (uint8_t)((NibbleFromChar(pSlot->Code[0]) << 4) | NibbleFromChar(pSlot->Code[1]));
    const uint8_t value = (uint8_t)((NibbleFromChar(pSlot->Code[2]) << 4) | NibbleFromChar(pSlot->Code[3]));
    const uint8_t addrLo = (uint8_t)((NibbleFromChar(pSlot->Code[4]) << 4) | NibbleFromChar(pSlot->Code[5]));
    const uint8_t addrHi = (uint8_t)((NibbleFromChar(pSlot->Code[6]) << 4) | NibbleFromChar(pSlot->Code[7]));
    const uint16_t addr  = (uint16_t)(((uint16_t)addrHi << 8) | addrLo);

    // Supported GameShark types for GB/GBC: 0x01 (plain write) and 0x91 (Crystal variant)
    if (!((type == 0x01u) || (type == 0x91u)))
    {
        pSlot->Valid = false;
        return false;
    }

    pSlot->Type  = type;
    pSlot->Value = value;
    pSlot->Addr  = addr;
    pSlot->Valid = true;

    return true;
}

static bool IsPlaceholderSlot(const CheatSlot_t* slot)
{
    if (slot == NULL)
    {
        return false;
    }
    return (!slot->Enabled) && (strncmp(slot->Code, "01000000", kCheat_CodeChars) == 0) && slot->Valid;
}

static uint8_t EffectiveSlotCount(void)
{
    return _Ctx.SlotCount;
}

static void PushSlotToFPGA(uint8_t idx, const CheatSlot_t* slot)
{
    if (idx >= kCheat_SlotsMax)
    {
        return;
    }

    const bool wasEnabled = _Ctx.LastEnabled[idx];
    const bool nowValid = (slot != NULL) && slot->Valid;
    const bool nowEnabled = nowValid && slot->Enabled;

    if (nowEnabled)
    {
        FPGA_Tx_SendCheatParsed(idx, true, slot->Type, slot->Addr, slot->Value);
        _Ctx.LastEnabled[idx] = true;
    }
    else if (wasEnabled)
    {
        // Send one disable to stop per-frame writes, then stay quiet until re-enabled.
        uint8_t type = nowValid ? slot->Type : 0x01u;
        uint16_t addr = nowValid ? slot->Addr : 0x0000u;
        uint8_t value = nowValid ? slot->Value : 0x00u;
        FPGA_Tx_SendCheatParsed(idx, false, type, addr, value);
        _Ctx.LastEnabled[idx] = false;
    }
}

static void PushSlotsToFPGA(uint8_t prevCount)
{
    const uint8_t effCount = EffectiveSlotCount();
    uint8_t maxSync = effCount;
    if (prevCount > maxSync)
    {
        maxSync = prevCount;
    }
    if (maxSync > kCheat_SlotsMax)
    {
        maxSync = kCheat_SlotsMax;
    }

    for (uint8_t i = 0; i < maxSync; i++)
    {
        const CheatSlot_t* slot = (i < effCount) ? &_Ctx.Slots[i] : NULL;
        PushSlotToFPGA(i, slot);
    }
}

static uint8_t NibbleFromChar(char c)
{
    if (c >= '0' && c <= '9')
    {
        return (uint8_t)(c - '0');
    }
    else if (c >= 'A' && c <= 'F')
    {
        return (uint8_t)(10 + c - 'A');
    }
    else if (c >= 'a' && c <= 'f')
    {
        return (uint8_t)(10 + c - 'a');
    }
    else
    {
        return 0;
    }
}

static char CharFromNibble(uint8_t n)
{
    n &= 0xF;
    if (n < 10)
    {
        return (char)('0' + n);
    }
    else
    {
        return (char)('A' + (n - 10));
    }
}

static void SendActiveToFPGA(void)
{
    if (EffectiveSlotCount() == 0)
    {
        return;
    }

    CheatSlot_t* pSlot = &_Ctx.Slots[_Ctx.ActiveIdx];
    if (!pSlot->Valid)
    {
        return;
    }

    FPGA_Tx_SendCheatParsed(_Ctx.ActiveIdx, pSlot->Enabled, pSlot->Type, pSlot->Addr, pSlot->Value);
}

static void TraceStatus(const char* action, const CheatSlot_t* pSlot)
{
    if ((action == NULL) || (pSlot == NULL))
    {
        return;
    }
}

__attribute__((constructor)) static void Cheats_Init(void)
{
    Cheats_Reset();
    Cheats_LoadPersist();
    _ReloadOnNextDraw = false;
}

void Cheats_SetGameKey(const char* key)
{
    const char* src = key;
    if ((src == NULL) || (src[0] == '\0'))
    {
        src = "default";
    }

    strncpy(_GameKey, src, sizeof(_GameKey));
    _GameKey[sizeof(_GameKey) - 1] = '\0';
    _PersistLoaded = false;
    _ReloadOnNextDraw = true; // reload cheats for new key on next draw
}

void Cheats_SetGameKeyFromTitle(const uint8_t title16[16])
{
    if (title16 == NULL)
    {
        Cheats_SetGameKey(NULL);
        return;
    }

    const uint32_t h = Cheats_HashTitle16(title16);
    Cheats_SetKeyFromHash(h);
}

void Cheats_SetGameHash(uint32_t hash_be)
{
    Cheats_SetKeyFromHash(hash_be);
}

static void Cheats_LoadPersist(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK)
    {
        return;
    }

    nvs_handle_t h;
    if (nvs_open("cheats", NVS_READONLY, &h) != ESP_OK)
    {
        return;
    }

    PersistCheats_t blob = {0};
    size_t required = sizeof(blob);
    err = nvs_get_blob(h, Cheats_GetKey(), &blob, &required);
    nvs_close(h);
    if (err != ESP_OK || required != sizeof(blob) || blob.Version != 1)
    {
        return;
    }

    if (blob.SlotCount > kCheat_SlotsMax)
    {
        return;
    }

    _Ctx.SlotCount = blob.SlotCount;
    _Ctx.ActiveIdx = (_Ctx.SlotCount > 0 && blob.ActiveIdx < blob.SlotCount) ? blob.ActiveIdx : 0;
    for (uint8_t i = 0; i < kCheat_SlotsMax; i++)
    {
        _Ctx.Slots[i] = blob.Slots[i];
    }

    // Normalize placeholder-only state back to zero slots
    if (EffectiveSlotCount() == 0)
    {
        _Ctx.SlotCount = 0;
        _Ctx.ActiveIdx = 0;
    }
    _PersistLoaded = true;
}

static void Cheats_SavePersist(void)
{
    if (!_PersistLoaded)
    {
        // Avoid writing defaults before we have a real user state
        if ((_Ctx.SlotCount <= 1) && !_Ctx.Slots[0].Enabled && strcmp(_Ctx.Slots[0].Code, "01000000") == 0)
        {
            return;
        }
    }

    if (!_Dirty)
    {
        return;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK)
    {
        return;
    }

    nvs_handle_t h;
    if (nvs_open("cheats", NVS_READWRITE, &h) != ESP_OK)
    {
        return;
    }

    PersistCheats_t blob = {0};
    blob.Version = 1;
    blob.SlotCount = EffectiveSlotCount();
    blob.ActiveIdx = (blob.SlotCount == 0) ? 0 : _Ctx.ActiveIdx;
    if (blob.SlotCount > kCheat_SlotsMax)
    {
        blob.SlotCount = kCheat_SlotsMax;
    }
    for (uint8_t i = 0; i < kCheat_SlotsMax; i++)
    {
        if (i < blob.SlotCount)
        {
            blob.Slots[i] = _Ctx.Slots[i];
        }
        else
        {
            memset(&blob.Slots[i], 0, sizeof(blob.Slots[i]));
        }
    }

    err = nvs_set_blob(h, Cheats_GetKey(), &blob, sizeof(blob));
    if (err == ESP_OK)
    {
        nvs_commit(h);
    }
    nvs_close(h);
    _PersistLoaded = true;
    _Dirty = false;
}

static const char* Cheats_GetKey(void)
{
    return _GameKey;
}

static void Cheats_SetKeyFromHash(uint32_t h)
{
    char buf[9];
    lv_snprintf(buf, sizeof(buf), "%08" PRIX32, (uint32_t)__builtin_bswap32(h));
    Cheats_SetGameKey(buf);
}

static uint32_t Cheats_HashTitle16(const uint8_t title16[16])
{
    // FNV-1a 32-bit over the 16-byte title
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < 16; i++)
    {
        h ^= title16[i];
        h *= 0x01000193u;
    }
    return h;
}

size_t Cheats_ImportCodes(const char *text, size_t len)
{
    if (text == NULL || len == 0)
    {
        return 0;
    }

    if (_Ctx.SlotCount > kCheat_SlotsMax)
    {
        _Ctx.SlotCount = kCheat_SlotsMax;
    }

    const uint8_t prevCount = EffectiveSlotCount();
    size_t added = 0;
    size_t removed = 0;

    const char *p = text;
    const char *end = text + len;

    while (p < end)
    {
        const char *line_end = p;
        while (line_end < end && *line_end != '\n' && *line_end != '\r')
        {
            line_end++;
        }

        const size_t line_len = (size_t)(line_end - p);
        char code[kCheat_CodeChars + 1] = {0};
        bool isRemove = false;
        if (NormalizeLineToCode(p, line_len, code, &isRemove))
        {
            if (isRemove)
            {
                const int idx = FindSlotIndex(code);
                if (idx >= 0)
                {
                    ShiftSlotsUp((uint8_t)idx);
                    if (_Ctx.SlotCount > 0)
                    {
                        _Ctx.SlotCount--;
                        ResetSlot(&_Ctx.Slots[_Ctx.SlotCount]);
                    }
                    removed++;
                }
            }
            else
            {
                const int existing = FindSlotIndex(code);
                if ((existing < 0) && (_Ctx.SlotCount < kCheat_SlotsMax))
                {
                    const bool replacePlaceholder = (_Ctx.SlotCount == 1) && IsPlaceholderSlot(&_Ctx.Slots[0]);
                    CheatSlot_t *slot = replacePlaceholder ? &_Ctx.Slots[0] : &_Ctx.Slots[_Ctx.SlotCount];
                    memset(slot, 0, sizeof(*slot));
                    memcpy(slot->Code, code, kCheat_CodeChars + 1);
                    // Default new imports to OFF; user can enable explicitly.
                    slot->Enabled = false;
                    if (ParseCheat(slot))
                    {
                        slot->Valid = true;
                        if (!replacePlaceholder)
                        {
                            _Ctx.SlotCount++;
                        }
                        added++;
                    }
                    else
                    {
                        ResetSlot(slot);
                    }
                }
            }
        }

        p = line_end;
        while (p < end && (*p == '\n' || *p == '\r'))
        {
            p++;
        }
    }

    if (_Ctx.SlotCount == 0)
    {
        _Ctx.ActiveIdx = 0;
    }
    else if (_Ctx.ActiveIdx >= _Ctx.SlotCount)
    {
        _Ctx.ActiveIdx = _Ctx.SlotCount - 1;
    }
    _Ctx.CursorIdx = 0;

    if ((added > 0) || (removed > 0))
    {
        _Dirty = true;
        _ReloadOnNextDraw = true;
        PushSlotsToFPGA(prevCount);
        Cheats_SavePersist();
        if (_Ctx.pPanelObj != NULL)
        {
            UpdateLabels();
        }
    }

    return added + removed;
}

size_t Cheats_ExportSlots(char *out, size_t outlen)
{
    if ((out == NULL) || (outlen == 0))
    {
        return 0;
    }

    size_t used = 0;
    const uint8_t effCount = EffectiveSlotCount();
    for (uint8_t i = 0; i < effCount; i++)
    {
        const char *code = _Ctx.Slots[i].Code;
        if (!IsValidCode(code))
        {
            continue;
        }

        // Need space for optional '\n' plus 8 chars and terminator
        const size_t need = kCheat_CodeChars + ((used > 0) ? 1u : 0u);
        if (used + need >= outlen)
        {
            break;
        }

        if (used > 0)
        {
            out[used++] = '\n';
        }

        memcpy(&out[used], code, kCheat_CodeChars);
        used += kCheat_CodeChars;
    }

    if (used < outlen)
    {
        out[used] = '\0';
    }
    else
    {
        out[outlen - 1] = '\0';
    }

    return used;
}

static bool IsHexChar(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}
