#include "sideband_spi.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "sideband_spi";

static spi_device_handle_t s_dev = NULL;
static gpio_num_t s_cs_gpio = GPIO_NUM_NC;

static uint16_t crc16_x25(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x0001u) {
                crc = (uint16_t)((crc >> 1) ^ 0x8408u);
            } else {
                crc >>= 1;
            }
        }
    }
    return (uint16_t)~crc;
}

esp_err_t SidebandSPI_Init(const sideband_spi_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cs_gpio = cfg->cs_gpio;

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << s_cs_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "cs gpio config");
    ESP_RETURN_ON_ERROR(gpio_set_level(s_cs_gpio, 1), TAG, "cs high");

    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = (int)cfg->clock_hz,
        .spics_io_num = -1, // manual CS toggle to avoid changing existing device
        .queue_size = 2,
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .flags = 0,
    };

    ESP_RETURN_ON_ERROR(spi_bus_add_device(cfg->host, &devcfg, &s_dev), TAG, "add device");
    return ESP_OK;
}

esp_err_t SidebandSPI_SendFrame(uint8_t opcode, const uint8_t *payload, size_t len)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((payload == NULL && len > 0) || len > 255) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t frame[4 + 255 + 2] = {0}; // opcode + len + payload + crc16
    const size_t header_len = 2; // opcode + length
    frame[0] = opcode;
    frame[1] = (uint8_t)len;
    if (len > 0) {
        memcpy(&frame[header_len], payload, len);
    }
    const size_t crc_off = header_len + len;
    const uint16_t crc = crc16_x25(frame, crc_off);
    frame[crc_off]     = (uint8_t)(crc & 0xFFu);
    frame[crc_off + 1] = (uint8_t)((crc >> 8) & 0xFFu);

    spi_transaction_t t = {0};
    t.length = (crc_off + 2) * 8;
    t.tx_buffer = frame;

    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(s_dev, portMAX_DELAY), TAG, "acquire bus");
    ESP_RETURN_ON_ERROR(gpio_set_level(s_cs_gpio, 0), TAG, "cs low");
    esp_err_t err = spi_device_polling_transmit(s_dev, &t);
    ESP_LOGV(TAG, "sent op=%u len=%u err=%d", opcode, (unsigned)len, err);
    gpio_set_level(s_cs_gpio, 1);
    spi_device_release_bus(s_dev);
    return err;
}
