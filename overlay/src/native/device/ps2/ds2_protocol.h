#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t buttons_lo;   /* active-low: Sel,L3,R3,Start,Up,Right,Down,Left */
    uint8_t buttons_hi;   /* active-low: L2,R2,L1,R1,Tri,Circle,Cross,Square */
    uint8_t rx, ry, lx, ly;
    uint8_t pressure[12]; /* R,L,U,D,Tri,Circle,Cross,Square,L1,R1,L2,R2 */
} ds2_pad_state_t;

typedef struct {
    bool config_mode;
    bool analog_mode;
    bool mode_locked;
    bool pressure_enabled;
    uint8_t rumble_map[6];
    uint8_t reply_mask[6];
    uint8_t button_attr[12];
    volatile uint8_t rumble_small;
    volatile uint8_t rumble_large;
    ds2_pad_state_t pad;
} ds2_device_t;

typedef struct {
    ds2_device_t *dev;
    bool addressed;
    bool config_at_start;
    uint8_t command;
    uint8_t byte_index;
    uint8_t tx_next;
    uint8_t old_rumble_map[6];
    uint8_t pending_rumble_map[6];
    uint8_t pending_reply_mask[6];
    uint8_t request[24];
    bool pending_config_valid;
    bool pending_config;
    bool pending_mode_valid;
    bool pending_analog;
    bool pending_lock_valid;
    bool pending_lock;
} ds2_session_t;

void ds2_device_init(ds2_device_t *d);
void ds2_set_pad_state(ds2_device_t *d, const ds2_pad_state_t *state);
uint8_t ds2_current_id(const ds2_device_t *d);
size_t ds2_current_poll_length(const ds2_device_t *d);

/* Full-duplex transaction helper.
 * Call ds2_session_begin() when ATT goes low.
 * Shift ds2_session_tx() while receiving the current command byte.
 * Then call ds2_session_rx() with that byte; it prepares the next TX byte.
 * Call ds2_session_end() when ATT goes high.
 */
void ds2_session_begin(ds2_session_t *s, ds2_device_t *d);
uint8_t ds2_session_tx(const ds2_session_t *s);
void ds2_session_rx(ds2_session_t *s, uint8_t rx);
void ds2_session_end(ds2_session_t *s);

/* Host-test helper: emulate an entire command packet. */
size_t ds2_exchange_packet(ds2_device_t *d, const uint8_t *cmd, size_t cmd_len,
                           uint8_t *reply, size_t reply_capacity);

#ifdef __cplusplus
}
#endif
