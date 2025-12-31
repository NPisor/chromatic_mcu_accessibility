#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    kFPGA_TxConsts_BufferSize = 1024,    // [bytes]
} FPGA_TxConsts_t;

void FPGA_TxTask(void *arg);
void FPGA_Tx_Resume(void);
void FPGA_Tx_Pause(void);
void FPGA_Tx_SendAll(void);
void FPGA_Tx_WriteBrightness(void);
void FPGA_Tx_SendSysCtl(void);
void FPGA_Tx_PokeButtons(void);
void FPGA_Tx_WritePaletteStyle(void);
void FPGA_Tx_WriteColorTemp(uint8_t level);
void FPGA_Tx_SendCheatParsed(uint8_t slot, bool enable, uint8_t type, uint16_t addr, uint8_t value);
void FPGA_Tx_SendCheatAscii(uint8_t slot, bool enable, const char code[9]);
void FPGA_Tx_RequestWRAMSnapshot(void);
void FPGA_Tx_RequestFBPreview(void);