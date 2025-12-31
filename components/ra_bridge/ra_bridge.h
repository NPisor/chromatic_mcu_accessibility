#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the placeholder RA bridge.
 * Sends a single GAME_INFO packet and starts periodic MEM_CHUNK sends.
 */
void RABridge_Init(void);

/** Set polling interval in milliseconds (minimum 100 ms). */
void RABridge_SetIntervalMs(uint32_t interval_ms);

/** Define a watch spec; cmp semantics are currently:
 * 0: always false (disabled), 1: any byte >= threshold within len triggers.
 */
void RABridge_SetWatch(uint8_t watch_id, uint16_t addr, uint8_t len, uint8_t cmp, uint8_t threshold);

/** Clear all watch specs. */
void RABridge_ClearWatches(void);

#ifdef __cplusplus
}
#endif
