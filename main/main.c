#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "gfx.h"
#include "board.h"
#include "brightness.h"
#include "color_correct_lcd.h"
#include "color_correct_usb.h"
#include "dpad_ctl.h"
#include "fpga_rx.h"
#include "fpga_tx.h"
#include "frameblend.h"
#include "fw.h"
#include "osd.h"
#include "osd_default.h"
#include "achievement.h"
#include "low_batt_icon_ctl.h"
#include "style.h"
#include "ble_bridge.h"
#include "player_num.h"
#include "screen_transit_ctl.h"
#include "mutex.h"
#include "serial_num.h"
#include "settings.h"
#include "silent.h"
#include "pwrmgr.h"
#include "ra_bridge.h"
#include <string.h>

enum {
    kLVGL_TickPeriod_us = 1000u, // 1 millisecond
};

enum {
    kFPGATxTask_StackDepth = 8*1024,   // [bytes]
    kFPGARxTask_StackDepth = 8*1024,   // [bytes]
    kSleepTask_StackDepth  = 4*1024,   // [bytes]
    kTimerTask_StackDepth  = 8*1024,   // [bytes]

    kFPGATxTask_Priority = configMAX_PRIORITIES - 10,
    kFPGARxTask_Priority = configMAX_PRIORITIES - 10,
    kSleepTask_Priority  = configMAX_PRIORITIES - 9,
};

static const char *TAG = "main";
static volatile uint8_t fpga_hsync = 0;

// Proxy opcodes (BLE characteristic e1f40400-78fc-4c5f-9aee-9f4d6a1d0004)
enum {
    kOpGameInfo      = 0x01,
    kOpMemChunk      = 0x02,
    kOpAchUnlock     = 0x03,
    kOpWatchSpec     = 0x10,
    kOpWatchClear    = 0x11,
    kOpGameId        = 0x12,
    kOpWatchHit      = 0x20,
    kOpUserProfile   = 0x30,
    kOpUserAvatarChunk = 0x31,
    kOpUserAvatarDone  = 0x32,
};

typedef struct ProfileRxCtx {
    char Username[64];
    uint32_t Points;
    uint8_t ScalePct;
    uint16_t Width;
    uint16_t Height;
    uint16_t NextChunk;
    size_t Len;
    bool HeaderSeen;
    uint8_t *pAvatarBuf;
    size_t AvatarCap;
} ProfileRxCtx_t;

static ProfileRxCtx_t sProfileRx;

static void lvgl_tick(void *arg);

static void persist_storage_init(void);
static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);
static void lvglTimerTask(void* param);
static void send_lines(spi_device_handle_t spi, int ypos, uint16_t *linedata);

static spi_device_handle_t spi;
static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
static lv_disp_drv_t disp_drv;      // contains callback functions
static lv_disp_t *disp;

static void proxy_rx_log(const uint8_t *data, size_t len)
{
    if (len == 0 || data == NULL)
    {
        return;
    }

    const uint8_t op = data[0];
    if (op == kOpAchUnlock && len > 1)
    {
        // ACH_UNLOCK payload: bytes after opcode are text/id (placeholder)
        char buf[80];
        size_t copy = (len - 1 < sizeof(buf) - 1) ? (len - 1) : (sizeof(buf) - 1);
        memcpy(buf, &data[1], copy);
        buf[copy] = '\0';
        ESP_LOGI(TAG, "BLE proxy unlock: %s", buf);
        return;
    }

    if (op == kOpWatchSpec)
    {
        if (len < 6)
        {
            ESP_LOGW(TAG, "WATCH_SPEC too short (%u)", (unsigned)len);
            return;
        }
        const uint8_t watch_id = data[1];
        const uint16_t addr = ((uint16_t)data[2] << 8) | data[3];
        const uint8_t span = data[4];
        const uint8_t cmp = data[5];
        const uint8_t threshold = (len > 6) ? data[6] : 0;
        RABridge_SetWatch(watch_id, addr, span, cmp, threshold);
        ESP_LOGI(TAG, "WATCH_SPEC id=%u addr=0x%04X len=%u cmp=%u thr=%u", (unsigned)watch_id, (unsigned)addr, (unsigned)span, (unsigned)cmp, (unsigned)threshold);
        return;
    }

    if (op == kOpWatchClear)
    {
        RABridge_ClearWatches();
        ESP_LOGI(TAG, "WATCH_CLEAR");
        return;
    }

    if (op == kOpUserProfile)
    {
        if (len < 9)
        {
            ESP_LOGW(TAG, "USER_PROFILE too short (%u)", (unsigned)len);
            return;
        }

        const uint32_t points = ((uint32_t)data[1] << 24) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 8) | data[4];
        const uint8_t scale = data[5];
        const uint16_t width = data[6];
        const uint16_t height = data[7];
        const uint8_t name_len = data[8];
        const size_t header_extra = 9;

        memset(&sProfileRx, 0, sizeof(sProfileRx));
        sProfileRx.Points = points;
        sProfileRx.ScalePct = scale;
        sProfileRx.Width = width;
        sProfileRx.Height = height;
        sProfileRx.NextChunk = 0;
        sProfileRx.HeaderSeen = true;

        uint8_t *pScratch = NULL;
        size_t scratchCap = 0;
        if (Achievement_GetAvatarScratch(&pScratch, &scratchCap) == kOSD_Result_Ok)
        {
            sProfileRx.pAvatarBuf = pScratch;
            sProfileRx.AvatarCap = scratchCap;
        }
        else
        {
            ESP_LOGW(TAG, "USER_PROFILE scratch alloc failed");
        }

        if (name_len > 0 && (header_extra + name_len) <= len)
        {
            const size_t copy = (name_len < sizeof(sProfileRx.Username) - 1) ? name_len : (sizeof(sProfileRx.Username) - 1);
            memcpy(sProfileRx.Username, &data[header_extra], copy);
            sProfileRx.Username[copy] = '\0';
        }
        else
        {
            strncpy(sProfileRx.Username, "PLAYER", sizeof(sProfileRx.Username) - 1);
            sProfileRx.Username[sizeof(sProfileRx.Username) - 1] = '\0';
        }

        AchievementUserProfile_t profile = {
            .pUsername = sProfileRx.Username,
            .Points = sProfileRx.Points,
            .pAvatar = NULL,
            .AvatarScalePct = sProfileRx.ScalePct,
        };
        (void)Achievement_SetUserProfile(&profile);

        ESP_LOGI(TAG, "USER_PROFILE u=%s pts=%lu w=%u h=%u scale=%u", sProfileRx.Username, (unsigned long)sProfileRx.Points, (unsigned)sProfileRx.Width, (unsigned)sProfileRx.Height, (unsigned)sProfileRx.ScalePct);
        return;
    }

    if (op == kOpUserAvatarChunk)
    {
        if (!sProfileRx.HeaderSeen || len < 4)
        {
            ESP_LOGW(TAG, "AVATAR_CHUNK ignored (header=%d len=%u)", (int)sProfileRx.HeaderSeen, (unsigned)len);
            return;
        }

        if ((sProfileRx.pAvatarBuf == NULL) || (sProfileRx.AvatarCap == 0))
        {
            ESP_LOGW(TAG, "AVATAR_CHUNK missing buffer");
            return;
        }

        const uint16_t chunk_id = ((uint16_t)data[1] << 8) | data[2];
        uint8_t chunk_len = data[3];
        if (chunk_len > (len - 4))
        {
            chunk_len = (uint8_t)(len - 4);
        }

        if (chunk_id != sProfileRx.NextChunk)
        {
            ESP_LOGW(TAG, "AVATAR_CHUNK out of order got=%u exp=%u", (unsigned)chunk_id, (unsigned)sProfileRx.NextChunk);
            return;
        }

        if ((sProfileRx.Len + chunk_len) > sProfileRx.AvatarCap)
        {
            ESP_LOGW(TAG, "AVATAR_CHUNK overflow len=%u total=%u", (unsigned)chunk_len, (unsigned)sProfileRx.Len);
            return;
        }

        memcpy(&sProfileRx.pAvatarBuf[sProfileRx.Len], &data[4], chunk_len);
        sProfileRx.Len += chunk_len;
        sProfileRx.NextChunk++;
        return;
    }

    if (op == kOpUserAvatarDone)
    {
        if (!sProfileRx.HeaderSeen)
        {
            ESP_LOGW(TAG, "AVATAR_DONE without header");
            return;
        }

        if (sProfileRx.Len == 0 || sProfileRx.Width == 0 || sProfileRx.Height == 0)
        {
            ESP_LOGW(TAG, "AVATAR_DONE missing data len=%u w=%u h=%u", (unsigned)sProfileRx.Len, (unsigned)sProfileRx.Width, (unsigned)sProfileRx.Height);
            sProfileRx.HeaderSeen = false;
            return;
        }

        if ((sProfileRx.pAvatarBuf == NULL) || (sProfileRx.AvatarCap == 0))
        {
            ESP_LOGW(TAG, "AVATAR_DONE missing buffer");
            sProfileRx.HeaderSeen = false;
            return;
        }

        (void)Achievement_UpdateFromRawImage(
            sProfileRx.Username,
            sProfileRx.Points,
            sProfileRx.pAvatarBuf,
            sProfileRx.Len,
            sProfileRx.Width,
            sProfileRx.Height,
            sProfileRx.ScalePct);

        ESP_LOGI(TAG, "AVATAR_DONE applied bytes=%u chunks=%u", (unsigned)sProfileRx.Len, (unsigned)sProfileRx.NextChunk);
        sProfileRx.HeaderSeen = false;
        return;
    }

    if (op == kOpGameId)
    {
        uint32_t game_id = 0;
        for (size_t i = 1; i < len && i < 5; i++)
        {
            game_id = (game_id << 8) | data[i];
        }
        ESP_LOGI(TAG, "GAME_ID announce: %u", (unsigned)game_id);
        return;
    }

    // Fallback log for other payloads
    char buf[64];
    size_t copy = (len < sizeof(buf) - 1) ? len : (sizeof(buf) - 1);
    memcpy(buf, data, copy);
    buf[copy] = '\0';
    ESP_LOGI(TAG, "BLE proxy rx (%u): %s", (unsigned)len, buf);
}

// Having some issue with floating point here so scale 9.6 10x
#define ROWS_PER_XFER_X10 32
#define NUMTRANS 1440/ROWS_PER_XFER_X10
#define PIXELSPERXFER 16*ROWS_PER_XFER_X10
#define BYTESPERXFER PIXELSPERXFER*2

static void notify_lvgl_flush_ready(spi_transaction_t *pTransaction)
{
    static int rownum = 0;

    (void)(pTransaction);

    rownum = rownum + ROWS_PER_XFER_X10;
    if(rownum >= 1440)
    {
        rownum = 0;
        lv_disp_flush_ready(&disp_drv);
    }
}

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    // copy a buffer's content to a specific area of the display
    send_lines(spi, 0, drv->draw_buf->buf1);
}

static void lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(kLVGL_TickPeriod_us);
}

static void lvglTimerTask(void* param)
{
    while(1)
    {
        // The task running lv_timer_handler should have lower priority than that running `lv_tick_inc`
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

DMA_ATTR lv_color_t buffy[160*144];

static void send_lines(spi_device_handle_t spi, int ypos, uint16_t *linedata)
{
    esp_err_t ret;

    int x;
    //Transaction descriptors. Declared static so they're not allocated on the stack; we need this memory even when this
    //function is finished because the SPI driver needs access to it even while we're already calculating the next line.
    static spi_transaction_t trans[NUMTRANS];

    //In theory, it's better to initialize trans and data only once and hang on to the initialized
    //variables. We allocate them on the stack, so we need to re-init them each call.
    for (x=0; x<NUMTRANS; x++) {
        memset(&trans[x], 0, sizeof(spi_transaction_t));
        trans[x].tx_buffer = &buffy[x*PIXELSPERXFER];//finally send the line datai
        trans[x].length = BYTESPERXFER * 8;  //Data length, in bits
        trans[x].flags = SPI_TRANS_MODE_QIO; //undo SPI_TRANS_USE_TXDATA flag
        trans[x].cmd = 0x400 | 1023;//write
        trans[x].addr = x*BYTESPERXFER;
    }

    //Queue all transactions.
    for (x=0; x<NUMTRANS; x++) {
        ret=spi_device_queue_trans(spi, &trans[x], portMAX_DELAY);
        assert(ret==ESP_OK);
    }

    return;
    //When we are here, the SPI driver is busy (in the background) getting the transactions sent. That happens
    //mostly using DMA, so the CPU doesn't have much to do here. We're not going to wait for the transaction to
    //finish because we may as well spend the time calculating the next line. When that is done, we can call
    //send_line_finish, which will wait for the transfers to be done and check their status.
}

static void register_update_callbacks(void)
{
    Brightness_RegisterOnUpdateCb(FPGA_Tx_WriteBrightness);
    PlayerNum_RegisterOnUpdateCb(FPGA_Tx_SendSysCtl);
    SilentMode_RegisterOnUpdateCb(FPGA_Tx_SendSysCtl);
    ColorCorrectLCD_RegisterOnUpdateCb(FPGA_Tx_SendSysCtl);
    ColorCorrectUSB_RegisterOnUpdateCb(FPGA_Tx_SendSysCtl);
    FrameBlend_RegisterOnUpdateCb(FPGA_Tx_SendSysCtl);
    ScreenTransitCtl_RegisterOnUpdateCb(FPGA_Tx_SendSysCtl);
    DPadCtl_RegisterOnUpdateCb(FPGA_Tx_SendSysCtl);
    LowBattIconCtl_RegisterOnUpdateCb(FPGA_Tx_SendSysCtl);
    Button_RegisterOnButtonPokeCb(FPGA_Tx_PokeButtons);
    Style_RegisterOnUpdateCb(FPGA_Tx_WritePaletteStyle);
}

static void persist_storage_init(void)
{
    Settings_Initialize();
    Firmware_Initialize();
    SerialNum_Initialize();
    Button_RegisterCommands();

    const fnSettingApply_t fnApplySetting[kNumSettingKeys] = {
        [kSettingKey_FrameBlend]       = FrameBlend_ApplySetting,
        [kSettingKey_ColorCorrectLCD]  = ColorCorrectLCD_ApplySetting,
        [kSettingKey_ColorCorrectUSB]  = ColorCorrectUSB_ApplySetting,
        [kSettingKey_PlayerNum]        = PlayerNum_ApplySetting,
        [kSettingKey_SilentMode]       = SilentMode_ApplySetting,
        [kSettingKey_Brightness]       = Brightness_ApplySetting,
        [kSettingKey_ScreenTransitCtl] = ScreenTransitCtl_ApplySetting,
        [kSettingKey_DPadCtl]          = DPadCtl_ApplySetting,
        [kSettingKey_LowBattIconCtl]   = LowBattIconCtl_ApplySetting,
        [kSettingKey_PaletteStyleID]   = Style_ApplySetting,
    };

    OSD_Result_t eResult;
    for(SettingKey_t k = kSettingKey_FirstKey; k < kNumSettingKeys; k++)
    {
        SettingValue_t Value = {
            .eType = kSettingDataType_U32,
            .U32 = 0,
        };

        const char* pSettingName = Settings_KeyToName(k);

        // Set up the data type eType for each key
        if ((eResult = Settings_RetrieveDefault(k, &Value)) != kOSD_Result_Ok)
        {
            ESP_LOGE(TAG, "Failed to setup %s with its default, err = %d", pSettingName, eResult);
            continue;
        }

        if ((eResult = Settings_Retrieve(k, &Value.U32)) != kOSD_Result_Ok)
        {
            ESP_LOGE(TAG, "Failed to load setting %s, err = %d. Using default value.", pSettingName, eResult);

            // Create the entry if not existent, or store the default
            if ((eResult = Settings_Update(k, Value.U32)) != kOSD_Result_Ok)
            {
                ESP_LOGE(TAG, "Failed to update setting %s on init, err = %d.", pSettingName, eResult);
            }
        }
        else
        {
            ESP_LOGI(TAG, "Setting loaded, %s = %lu", pSettingName, Value.U32);
        }

        // Send over the setting to the module
        if (fnApplySetting[k] != NULL)
        {
            if ((eResult = fnApplySetting[k](&Value)) != kOSD_Result_Ok)
            {
                ESP_LOGE(TAG, "Failed to apply setting %s to %lu, err = %d.", pSettingName, Value.U32, eResult);
            }
        }
    }
}

static esp_console_repl_t *pRepl = NULL;
static esp_console_repl_config_t ReplConfig = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
static esp_console_dev_uart_config_t ReplHWConfig = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

void app_main(void)
{
    persist_storage_init();
    Mutex_Init();

    BLEBridge_Init();
    BLEBridge_RegisterProxyRxCb(proxy_rx_log);
    RABridge_Init();

    // Set up the system management UART to/from the FPGA
    ESP_LOGI(TAG, "Initialize FPGA UART");
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_driver_install(UART_NUM_1, kFPGA_TxConsts_BufferSize, kFPGA_RxConsts_BufferSize, 0, NULL, 0);
    uart_param_config(UART_NUM_1, &uart_config);
    uart_set_pin(UART_NUM_1, PIN_NUM_UART_TO_FPGA, PIN_NUM_UART_FROM_FPGA, -1, -1);

    gpio_sleep_set_direction(PIN_NUM_UART_FROM_FPGA, GPIO_MODE_INPUT);
    gpio_sleep_set_pull_mode(PIN_NUM_UART_FROM_FPGA, GPIO_PULLUP_ONLY);

    // Task is created earlier than the others to apply the settings ASAP
    xTaskCreate(FPGA_TxTask, "fpga_tx_task", kFPGATxTask_StackDepth, NULL, kFPGATxTask_Priority, FPGA_GetTxTaskHandle());

    // Task is created early in order to recieve fast messages (e.g hotkey palette data)
    // These tasks are started in the "paused" state
    xTaskCreate(FPGA_RxTask, "fpga_rx_task", kFPGARxTask_StackDepth, NULL, kFPGARxTask_Priority, FPGA_GetRxTaskHandle());

    // Let the tasks run for a bit to transmit config data to the FPGA core
    const size_t kTransmitCfgCounts = 6;
    for(size_t i = 0; i < kTransmitCfgCounts; i++)
    {
        FPGA_Tx_SendAll();
        vTaskDelay( pdMS_TO_TICKS(10) );
    }

    ESP_LOGI(TAG, "Initialize SPI Master");

    spi_bus_config_t buscfg={
        .miso_io_num=PIN_NUM_QSPI_MISO,
        .mosi_io_num=PIN_NUM_QSPI_MOSI,
        .sclk_io_num=PIN_NUM_QSPI_CLK,
        .quadwp_io_num=PIN_NUM_QSPI_WP,
        .quadhd_io_num=PIN_NUM_QSPI_HD,
        .max_transfer_sz=(BYTESPERXFER*8)+46,
        .flags=SPICOMMON_BUSFLAG_QUAD | SPICOMMON_BUSFLAG_MASTER
    };
    spi_device_interface_config_t devcfg={
        .clock_speed_hz=40*1000*1000,           //Clock out at 40 MHz
        .mode=0,                                //SPI mode 0
        .spics_io_num=PIN_NUM_QSPI_CS,
        .queue_size=NUMTRANS,
        .pre_cb=NULL,
        .flags=SPI_DEVICE_HALFDUPLEX,
        .cs_ena_pretrans=3,
        .cs_ena_posttrans=3,
        .command_bits=11,
        .address_bits=32,
        .dummy_bits=3,
        .post_cb=notify_lvgl_flush_ready
    };

    //Initialize the SPI bus
    esp_err_t ret=spi_bus_initialize(VSPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
    ret=spi_bus_add_device(VSPI_HOST, &devcfg, &spi);
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    // alloc draw buffers used by LVGL
    // it's recommended to choose the size of the draw buffer(s) to be at least 1/10 screen sized
    lv_color_t *buf1 = (lv_color_t*)&buffy;
    assert(buf1);
    lv_disp_draw_buf_init(&disp_buf, buf1, NULL, kOSD_NumPixels);

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = kOSD_Width_px;
    disp_drv.ver_res = kOSD_Height_px;
    disp_drv.direct_mode = 1;
    disp_drv.full_refresh = 1;
    disp_drv.flush_cb = example_lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp = lv_disp_drv_register(&disp_drv);

    //Configuration is completed.

    ESP_LOGI(TAG, "Install LVGL tick timer");
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, kLVGL_TickPeriod_us));

    ESP_LOGI(TAG, "Display LVGL animation");
    lv_obj_t *scr = lv_disp_get_scr_act(disp);

    OSD_Initialize();
    OSD_Default_Init(scr);
    register_update_callbacks();

    Gfx_Start(scr);

    xTaskCreatePinnedToCore(lvglTimerTask, "lvgl Timer", kTimerTask_StackDepth, NULL, 4, NULL, 1);

    // Prompt to be printed before each line.
    ReplConfig.prompt = "mcu> ";
    ReplConfig.max_cmdline_length = 1024;

    ReplHWConfig.rx_gpio_num = GPIO_NUM_35;
    ReplHWConfig.tx_gpio_num = GPIO_NUM_36;

    ESP_ERROR_CHECK(esp_console_new_repl_uart(&ReplHWConfig, &ReplConfig, &pRepl));
    esp_console_register_help_command();
    // All commands must be registered prior to starting the REPL
    ESP_ERROR_CHECK(esp_console_start_repl(pRepl));

    vTaskDelay( pdMS_TO_TICKS(2000) );
    xTaskCreate(PwrMgr_Task, "pwr_mgr_task", kSleepTask_StackDepth, NULL, kSleepTask_Priority, NULL);
}
