#pragma once

#include <stdint.h>
#include "core/output_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const OutputInterface ps2_output_interface;

uint8_t ps2_rumble_large(void);
uint8_t ps2_rumble_small(void);
uint32_t ps2_transaction_count(void);
uint32_t ps2_rx_byte_count(void);
uint32_t ps2_address_count(void);
uint32_t ps2_poll42_count(void);

#ifdef __cplusplus
}
#endif
