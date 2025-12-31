#include "fpga_tx.h"

#include "battery.h"
#include "brightness.h"
#include "color_correct_lcd.h"
#include "color_correct_usb.h"
#include "crc8_sae_j1850.h"
#include "dpad_ctl.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "fpga_common.h"
#include "frameblend.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "low_batt_icon_ctl.h"
#include "palette.h"
#include "player_num.h"
#include "screen_transit_ctl.h"
#include "silent.h"
#include "style.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

enum {
    kFlag_Resume,
    kFlag_SetBrightness,
    kFlag_SetSysCtl,
    kFlag_RequestFWVer,
    kFlag_PokeButton,
    kFlag_SetPaletteStyle,
    kFlag_SetColorTemp,
    kFlag_RequestBGPD,
    kFlag_SendCheat,
    kNumFlags,

    kFlag_RequestWRAMSnapshot,
    kFlag_RequestFBPreview,
};

typedef enum {
    kTxFlag_Resume           = (1 << kFlag_Resume),
    kTxFlag_WriteBrightness  = (1 << kFlag_SetBrightness),
    kTxFlag_SetSysCtl        = (1 << kFlag_SetSysCtl),
    kTxFlag_RequestFWVer     = (1 << kFlag_RequestFWVer),
    kTxFlag_PokeButton       = (1 << kFlag_PokeButton),
    kTxFlag_SetPaletteStyle  = (1 << kFlag_SetPaletteStyle),
    kTxFlag_SetColorTemp     = (1 << kFlag_SetColorTemp),
    kTxFlag_RequestBGPD      = (1 << kFlag_RequestBGPD),
    kTxFlag_SendCheat        = (1 << kFlag_SendCheat),
    kTxFlag_RequestWRAMSnap  = (1 << kFlag_RequestWRAMSnapshot),
    kTxFlag_RequestFBPreview = (1 << kFlag_RequestFBPreview),

    kTxFlag_AllFlags         = ((1 << kNumFlags) - 1),
} TxFlags_t;

typedef enum {
    kTxCmd_Reserved0        = 0x0,
    kTxCmd_Reserved1        = 0x1,
    kTxCmd_Reserved2        = 0x2,
    kTxCmd_Reserved3        = 0x3,
    kTxCmd_SysCtrl          = 0x4,
    kTxCmd_BacklightCtl     = 0x5,
    kTxCmd_ReqFWVer         = 0x6,
    kTxCmd_PokeButton       = 0x9,
    kTxCmd_BGPaletteCtl     = 0xB,
    kTxCmd_SpritePaletteCtl = 0xC,
    kTxCmd_ReqBGPD          = 0xD,
    kTxCmd_CheatPoke        = 0xE,
    kTxCmd_ReqWRAMSnapshot  = 0xF,
    kTxCmd_ReqFBPreview     = 0x10,
    kTxCmd_GBCColorTemp     = 0x11,

    kNumTxCmds,
} TxIDs_t;

// Declare a variable to hold the handle of the created event group.
static EventGroupHandle_t xEventGroupHandle;

// Declare a variable to hold the data associated with the created event group.
static StaticEventGroup_t xCreatedEventGroup;
static const char* TAG = "FpgaTx";

enum {
    kCheat_MaxSlotsTx = 8,
};

typedef struct __attribute__((packed)) CheatPayload {
    uint8_t Slot;
    uint8_t TypeFlags; // bit7: enable, bits6:0 type code
    uint8_t AddrHi;
    uint8_t AddrLo;
    uint8_t Value;
    uint8_t Compare;
} CheatPayload_t;

static struct {
    CheatPayload_t Payload;
    bool Pending;
} CheatTx;

static uint8_t sColorTempLevel = 0; // neutral default

static size_t SetupTxBuffer(uint8_t *const pBuffer, TxIDs_t eID, uint8_t Len, void* pData);

void FPGA_TxTask(void *arg)
{
    (void)arg;

    xEventGroupHandle = xEventGroupCreateStatic( &xCreatedEventGroup );

    FPGA_Tx_SendAll();
    FPGA_Tx_Resume();

    uint8_t TxBuffer[14] = {0};

    while (1)
    {
        //Only run when we're given permission to do so.
        (void) xEventGroupWaitBits(
            xEventGroupHandle,
            kTxFlag_Resume,
            pdFALSE,        // Do not clear the Resume flag
            pdTRUE,         // Wait for this bit
            portMAX_DELAY
        );

        const EventBits_t EventBits = xEventGroupWaitBits(
            xEventGroupHandle,
            (kTxFlag_WriteBrightness | kTxFlag_SetSysCtl | kTxFlag_RequestFWVer | kTxFlag_PokeButton | kTxFlag_SetPaletteStyle | kTxFlag_SetColorTemp | kTxFlag_RequestBGPD | kTxFlag_SendCheat | kTxFlag_RequestWRAMSnap | kTxFlag_RequestFBPreview),
            pdTRUE, // DO clear the flags to complete the request
            pdFALSE, // Any bit will do
            pdMS_TO_TICKS(100)
        );

        if ((EventBits & kTxFlag_WriteBrightness) == kTxFlag_WriteBrightness)
        {
            const uint16_t MaxDisplayBrightness = 16;
            uint16_t Backlight = (uint16_t)Brightness_GetLevel();
            if (Backlight < MaxDisplayBrightness)
            {
                const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_BacklightCtl, sizeof(Backlight), (void*)&Backlight);
                (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);
            }
        }

        if ((EventBits & kTxFlag_SetSysCtl) == kTxFlag_SetSysCtl)
        {
            const uint16_t frame_blending = (uint8_t)(FrameBlend_GetState() == kFrameBlendState_On);
            const uint16_t ismuted        = (uint8_t)(SilentMode_GetState() == kSilentModeState_On);
            const uint16_t playernum      = PlayerNum_GetNum();
            const uint16_t color_correct  = (
                ((uint16_t)(ColorCorrectLCD_GetState() == kColorCorrectLCDState_On) << 0) |
                ((uint16_t)(ColorCorrectUSB_GetState() == kColorCorrectUSBState_On) << 1)
            );
            const uint16_t IgnoreDiagonalInputs = (uint16_t)(DPadCtl_GetState() == kDPadCtlState_RejectDiag);
            const uint16_t EnableScreenTransitionFix  = (uint16_t)(ScreenTransitCtl_GetState() == kScreenTransitCtlState_On);
            const uint16_t LowBattIconControl = (uint16_t)(LowBattIconCtl_GetState());

            const uint16_t Payload = ( (frame_blending << 1) | (color_correct << 2) | ismuted | (playernum << 4) | (EnableScreenTransitionFix << 12) | (IgnoreDiagonalInputs << 11) | (LowBattIconControl << 13));
            const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_SysCtrl, sizeof(Payload), (void*)&Payload);
            (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);
        }

        if ((EventBits & kTxFlag_RequestFWVer) == kTxFlag_RequestFWVer)
        {
            uint16_t dummy = 0;
            const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_ReqFWVer, sizeof(dummy), &dummy);
            (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);
        }

        if ((EventBits & kTxFlag_PokeButton) == kTxFlag_PokeButton)
        {
            const uint16_t PokedButtons = Button_GetPokedInputs();
            const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_PokeButton, sizeof(PokedButtons), (void*)&PokedButtons);
            (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);
        }

        if ((EventBits & kTxFlag_RequestBGPD) == kTxFlag_RequestBGPD)
        {
            uint16_t dummy = 0;
            const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_ReqBGPD, sizeof(dummy), &dummy);
            (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);
        }

        if ((EventBits & kTxFlag_RequestWRAMSnap) == kTxFlag_RequestWRAMSnap)
        {
            uint16_t dummy = 0;
            const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_ReqWRAMSnapshot, sizeof(dummy), &dummy);
            (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);
            ESP_LOGI(TAG, "Sent WRAM snapshot request");
        }

        if ((EventBits & kTxFlag_RequestFBPreview) == kTxFlag_RequestFBPreview)
        {
            uint16_t dummy = 0;
            const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_ReqFBPreview, sizeof(dummy), &dummy);
            (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);
            ESP_LOGI(TAG, "Sent FB preview request");
        }

        if ((EventBits & kTxFlag_SetPaletteStyle) == kTxFlag_SetPaletteStyle)
        {
            if (Style_IsGBCMode())
            {
                ESP_LOGD(TAG, "Skip palette write: GBC mode active");
            }
            else
            {
                if (!Style_IsInitialized())
                {
                    // Prevent palette from being sent fast on initial SendAll()s
                    // so that there is time to read back the hotkey data from FPGA
                    vTaskDelay( pdMS_TO_TICKS(50) );
                    Style_Initialize();
                }

                const StyleID_t ID = Style_GetCurrID();
                const uint64_t ColorBG = Pal_GetColor(ID, kPalette_Bg);
                // toggle custom palette enable bit
                const uint64_t PayloadBG = __builtin_bswap64(ColorBG ^ ((uint64_t)1 << kCustomPaletteEn));

                // BG
                const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_BGPaletteCtl, sizeof(PayloadBG), (void*)&PayloadBG);
                (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);

                // Sprite - Obj0
                const uint64_t ColorObj0 = Pal_GetColor(ID, kPalette_Obj0);
                const uint64_t PayloadObj0 = __builtin_bswap64(ColorObj0);
                const size_t Size2 = SetupTxBuffer(TxBuffer, kTxCmd_SpritePaletteCtl, sizeof(PayloadObj0), (void*)&PayloadObj0);
                (void)uart_write_bytes(UART_NUM_1, TxBuffer, Size2);

                // Sprite - Obj1
                const uint64_t ColorObj1 = Pal_GetColor(ID, kPalette_Obj1);
                const uint64_t PayloadObj1 = __builtin_bswap64(ColorObj1 | ((uint64_t)1 << kCustomPaletteObjSel));
                const size_t Size3 = SetupTxBuffer(TxBuffer, kTxCmd_SpritePaletteCtl, sizeof(PayloadObj1), (void*)&PayloadObj1);
                (void)uart_write_bytes(UART_NUM_1, TxBuffer, Size3);
            }
        }

        if ((EventBits & kTxFlag_SetColorTemp) == kTxFlag_SetColorTemp)
        {
            const uint16_t payload = (uint16_t)sColorTempLevel;
            const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_GBCColorTemp, sizeof(payload), (void*)&payload);
            (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);
        }

        if ((EventBits & kTxFlag_SendCheat) == kTxFlag_SendCheat)
        {
            if (CheatTx.Pending)
            {
                CheatPayload_t Payload = CheatTx.Payload;
                const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_CheatPoke, sizeof(Payload), (void*)&Payload);
                (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);
                CheatTx.Pending = false;
            }
        }

        memset(TxBuffer, 0x0, sizeof(TxBuffer));
    }

    ESP_LOGE(TAG, "TxTask loop exited");
}

void FPGA_Tx_Resume(void)
{
   (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_Resume);
}

void FPGA_Tx_Pause(void)
{
   (void) xEventGroupClearBits(xEventGroupHandle, kTxFlag_Resume);
}

void FPGA_Tx_SendAll(void)
{
   (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_AllFlags);
}

void FPGA_Tx_WriteBrightness(void)
{
   (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_WriteBrightness);
}

void FPGA_Tx_SendSysCtl(void)
{
   (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_SetSysCtl);
}

void FPGA_Tx_PokeButtons(void)
{
   (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_PokeButton);
}

void FPGA_Tx_WritePaletteStyle(void)
{
   (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_SetPaletteStyle);
}

void FPGA_Tx_WriteColorTemp(uint8_t level)
{
    sColorTempLevel = level;
    if (xEventGroupHandle != NULL)
    {
        (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_SetColorTemp);
    }
}

void FPGA_Tx_SendCheatParsed(uint8_t slot, bool enable, uint8_t type, uint16_t addr, uint8_t value)
{
    if (slot >= kCheat_MaxSlotsTx)
    {
        return;
    }

    memset(&CheatTx.Payload, 0x0, sizeof(CheatTx.Payload));
    CheatTx.Payload.Slot      = slot;
    CheatTx.Payload.TypeFlags = (uint8_t)((type & 0x7Fu) | (enable ? 0x80u : 0x00u));
    CheatTx.Payload.AddrHi    = (uint8_t)((addr >> 8) & 0xFFu);
    CheatTx.Payload.AddrLo    = (uint8_t)(addr & 0xFFu);
    CheatTx.Payload.Value     = value;
    CheatTx.Payload.Compare   = (type == 0x91u) ? value : 0x00u;

    CheatTx.Pending = true;

    if (xEventGroupHandle != NULL)
    {
        (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_SendCheat);
    }
}

void FPGA_Tx_SendCheatAscii(uint8_t slot, bool enable, const char code[9])
{
    if ((code == NULL) || (slot >= kCheat_MaxSlotsTx))
    {
        return;
    }

    // Expect 8 hex digits: TT VV AA AA
    uint8_t parsed[8] = {0};
    for (size_t i = 0; i < 8; i++)
    {
        if (!isxdigit((unsigned char)code[i]))
        {
            return;
        }
        char c = (char)toupper((unsigned char)code[i]);
        parsed[i] = (uint8_t)((c >= 'A') ? (10 + c - 'A') : (c - '0'));
    }

    const uint8_t type   = (uint8_t)((parsed[0] << 4) | parsed[1]);
    const uint8_t value  = (uint8_t)((parsed[2] << 4) | parsed[3]);
    const uint16_t addr  = (uint16_t)(((uint16_t)((parsed[6] << 4) | parsed[7]) << 8) | ((uint16_t)((parsed[4] << 4) | parsed[5])));

    FPGA_Tx_SendCheatParsed(slot, enable, type, addr, value);
}

void FPGA_Tx_RequestWRAMSnapshot(void)
{
    if (xEventGroupHandle != NULL)
    {
        (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_RequestWRAMSnap);
    }
}

void FPGA_Tx_RequestFBPreview(void)
{
    if (xEventGroupHandle != NULL)
    {
        (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_RequestFBPreview);
    }
}

static size_t SetupTxBuffer(uint8_t *const pBuffer, TxIDs_t eID, uint8_t Len, void* pData)
{
    if ((pBuffer == NULL) || (Len > kSysMgmtConsts_MsgProtoV2Len) || (pData == NULL) || ((unsigned)eID >= kNumTxCmds))
    {
        return 0;
    }

    pBuffer[0] = (uint8_t)(FPGA_IsProtoV1() ? kSysMgmtConsts_HeaderV1Marker : kSysMgmtConsts_HeaderV2Marker);
    pBuffer[1] = (uint8_t)eID;

    size_t MsgSize = 4;
    size_t PayloadOffset = 2;
    if (pBuffer[0] == kSysMgmtConsts_HeaderV2Marker)
    {
        PayloadOffset++;
    }

    if ((pData != NULL) && (Len > 0))
    {
        // 16-bit data is sent in big endian
        if (Len == 2)
        {
            uint16_t *const  pu16Data = (uint16_t *const)pData;
            *pu16Data = __builtin_bswap16(*pu16Data);
        }

        memcpy(&pBuffer[PayloadOffset], pData, Len);
    }

    if (pBuffer[0] == kSysMgmtConsts_HeaderV2Marker)
    {
        pBuffer[2] = Len;
        MsgSize = crc8_sae_j1850_encode(pBuffer, 3 + Len, pBuffer);
    }

    return MsgSize;
}
