#include "ra_bridge.h"

#include <string.h>

#include "ble_bridge.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#define OP_GAME_INFO 0x01
#define OP_MEM_CHUNK 0x02
#define OP_ACH_UNLOCK 0x03
#define OP_WATCH_HIT 0x20

// Placeholder address window
#define RA_ADDR_BASE 0xC000
#define RA_ADDR_LEN  0x0040 // 64 bytes

static TimerHandle_t s_timer = NULL;
static uint32_t s_interval_ms = 1000;
static uint8_t s_last_buf[RA_ADDR_LEN];
static bool s_have_last = false;
static const char *TAG = "RABridge";

typedef struct {
    bool in_use;
    uint8_t watch_id;
    uint16_t addr;
    uint8_t len;
    uint8_t cmp;
    uint8_t threshold;
} WatchSpec;

static WatchSpec s_watches[8];
static size_t s_watch_count = 0;

static bool send_game_info(void)
{
    uint8_t payload[64];
    const char game_str[] = "DUMMY_GAME";
    size_t len = 0;
    payload[len++] = OP_GAME_INFO;
    memcpy(&payload[len], game_str, sizeof(game_str) - 1);
    len += sizeof(game_str) - 1;
    payload[len++] = 0x01; // platform placeholder
    int rc = BLEBridge_SendProxy(payload, (uint16_t)len);
    if (rc == 0)
    {
        ESP_LOGI(TAG, "GAME_INFO sent (%u bytes)", (unsigned)len);
        return true;
    }
    ESP_LOGW(TAG, "GAME_INFO send failed rc=%d", rc);
    return false;
}

static bool send_mem_chunk(const uint8_t *buf, uint16_t len)
{
    if (len == 0 || len > 180)
    {
        return false;
    }
    uint8_t payload[192];
    uint16_t p = 0;
    payload[p++] = OP_MEM_CHUNK;
    payload[p++] = (uint8_t)(RA_ADDR_BASE >> 8);
    payload[p++] = (uint8_t)(RA_ADDR_BASE & 0xFF);
    payload[p++] = (uint8_t)len;
    memcpy(&payload[p], buf, len);
    p += len;
    int rc = BLEBridge_SendProxy(payload, p);
    if (rc == 0)
    {
        ESP_LOGD(TAG, "MEM_CHUNK sent len=%u", (unsigned)len);
        return true;
    }
    ESP_LOGW(TAG, "MEM_CHUNK send failed rc=%d", rc);
    return false;
}

static void send_watch_hit(const WatchSpec *w, const uint8_t *buf)
{
    if (w == NULL || buf == NULL)
    {
        return;
    }
    uint8_t payload[16 + RA_ADDR_LEN];
    uint16_t p = 0;
    payload[p++] = OP_WATCH_HIT;
    payload[p++] = w->watch_id;
    payload[p++] = (uint8_t)(w->addr >> 8);
    payload[p++] = (uint8_t)(w->addr & 0xFF);
    payload[p++] = w->len;
    const uint8_t copy_len = w->len > RA_ADDR_LEN ? RA_ADDR_LEN : w->len;
    memcpy(&payload[p], buf, copy_len);
    p += copy_len;
    (void)BLEBridge_SendProxy(payload, p);
}

static void tick_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    static bool game_sent = false;
    if (!game_sent)
    {
        game_sent = send_game_info();
    }

    // Generate deterministic pseudo data for now.
    static uint8_t counter = 0;
    uint8_t buf[RA_ADDR_LEN];
    for (uint16_t i = 0; i < RA_ADDR_LEN; i++)
    {
        buf[i] = (uint8_t)(counter + i);
    }
    counter++;

    bool changed = false;
    if (!s_have_last || memcmp(buf, s_last_buf, RA_ADDR_LEN) != 0)
    {
        changed = true;
    }

    if (changed)
    {
        if (send_mem_chunk(buf, RA_ADDR_LEN))
        {
            memcpy(s_last_buf, buf, RA_ADDR_LEN);
            s_have_last = true;
        }
    }

    // Evaluate watches: cmp==1 => any byte >= threshold within len
    for (size_t i = 0; i < s_watch_count; i++)
    {
        if (!s_watches[i].in_use)
        {
            continue;
        }
        const WatchSpec *w = &s_watches[i];
        if (w->addr != RA_ADDR_BASE)
        {
            continue; // placeholder window only
        }
        const uint8_t span = (w->len > RA_ADDR_LEN) ? RA_ADDR_LEN : w->len;
        bool hit = false;
        if (w->cmp == 1)
        {
            for (uint8_t j = 0; j < span; j++)
            {
                if (buf[j] >= w->threshold)
                {
                    hit = true;
                    break;
                }
            }
        }
        if (hit)
        {
            send_watch_hit(w, buf);
        }
    }
}

void RABridge_Init(void)
{
    if (s_timer != NULL)
    {
        return;
    }

    // Silence verbose RA bridge logging to keep console clean
    esp_log_level_set(TAG, ESP_LOG_NONE);

    s_timer = xTimerCreate("ra_br", pdMS_TO_TICKS(s_interval_ms), pdTRUE, NULL, tick_cb);
    if (s_timer != NULL)
    {
        xTimerStart(s_timer, 0);
    }
}

void RABridge_SetIntervalMs(uint32_t interval_ms)
{
    if (interval_ms < 100)
    {
        interval_ms = 100;
    }
    s_interval_ms = interval_ms;
    if (s_timer != NULL)
    {
        xTimerChangePeriod(s_timer, pdMS_TO_TICKS(s_interval_ms), 0);
    }
}

void RABridge_SetWatch(uint8_t watch_id, uint16_t addr, uint8_t len, uint8_t cmp, uint8_t threshold)
{
    size_t slot = s_watch_count;
    for (size_t i = 0; i < s_watch_count; i++)
    {
        if (s_watches[i].in_use && s_watches[i].watch_id == watch_id)
        {
            slot = i;
            break;
        }
    }
    if (slot >= (sizeof(s_watches) / sizeof(s_watches[0])))
    {
        return;
    }
    s_watches[slot] = (WatchSpec){
        .in_use = true,
        .watch_id = watch_id,
        .addr = addr,
        .len = len,
        .cmp = cmp,
        .threshold = threshold,
    };
    if (slot == s_watch_count)
    {
        s_watch_count++;
    }
}

void RABridge_ClearWatches(void)
{
    memset(s_watches, 0, sizeof(s_watches));
    s_watch_count = 0;
}
