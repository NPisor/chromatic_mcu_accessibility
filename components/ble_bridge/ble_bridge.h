#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    kBLEBridge_State_Stopped = 0,
    kBLEBridge_State_Advertising,
    kBLEBridge_State_Connected,
} BLEBridge_State_t;

typedef void (*BLEBridge_ProxyRxCb_t)(const uint8_t *data, size_t len);

typedef void (*BLEBridge_StateCb_t)(BLEBridge_State_t state);

void BLEBridge_Init(void);
void BLEBridge_StartAdvertising(void);
void BLEBridge_StopAdvertising(void);
BLEBridge_State_t BLEBridge_GetState(void);
void BLEBridge_RegisterStateCb(BLEBridge_StateCb_t cb);
void BLEBridge_SetEnabled(bool enabled);
bool BLEBridge_IsEnabled(void);
int BLEBridge_GetLastAdvError(void);
const char *BLEBridge_GetLastAdvErrorStr(void);
const char *BLEBridge_GetAddrStr(void);

/* Utility exposed for testing/demo: push a test payload to notifications. */
void BLEBridge_PushStatusNotify(const uint8_t *data, uint8_t len);

/* Proxy channel: MCU -> host via notify, host -> MCU via write. */
int BLEBridge_SendProxy(const uint8_t *data, uint16_t len);
void BLEBridge_RegisterProxyRxCb(BLEBridge_ProxyRxCb_t cb);
