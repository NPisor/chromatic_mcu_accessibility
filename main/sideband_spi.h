#pragma once

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t host;
    gpio_num_t cs_gpio;
    uint32_t clock_hz;
} sideband_spi_config_t;

esp_err_t SidebandSPI_Init(const sideband_spi_config_t *cfg);
esp_err_t SidebandSPI_SendFrame(uint8_t opcode, const uint8_t *payload, size_t len);

#ifdef __cplusplus
}
#endif
