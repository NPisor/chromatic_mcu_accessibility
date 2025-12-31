#pragma once

#include "osd_shared.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

OSD_Result_t Cheats_Draw(void* arg);
OSD_Result_t Cheats_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
OSD_Result_t Cheats_OnTransition(void* arg);

/* Reset all cheat slots back to zeroed codes and disabled state. */
void Cheats_Reset(void);

// Import cheat codes from a text buffer (one 8-char hex code per line). Returns number of codes applied.
size_t Cheats_ImportCodes(const char *text, size_t len);

// Export current slots as newline-delimited 8-char codes (max outlen-1). Returns bytes written (excluding terminator).
size_t Cheats_ExportSlots(char *out, size_t outlen);

/* True when edit mode is active so navigation can be captured. */
bool Cheats_IsInEdit(void);
bool Cheats_HasEnabled(void);
// Set the active game key (e.g., hash/ID) for per-game cheat persistence.
// Provide a stable, short ASCII identifier (up to 16 chars). Falls back to "default" if NULL/empty.
void Cheats_SetGameKey(const char* key);
// Convenience: set key from the 16-byte Game Boy title at 0x0134-0x0143 (ASCII, padded with 0).
// This runs a small hash over the title to produce a stable 8-char hex key.
void Cheats_SetGameKeyFromTitle(const uint8_t title16[16]);
// Convenience: set key directly from a 32-bit hash (already computed elsewhere).
void Cheats_SetGameHash(uint32_t hash_be);
