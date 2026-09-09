#include "ds2_protocol.h"
#include <string.h>

static uint8_t poll_reply_byte(const ds2_device_t *d, uint8_t pos) {
    if (pos == 3) return d->pad.buttons_lo;
    if (pos == 4) return d->pad.buttons_hi;
    if (!d->analog_mode) return 0xFF;
    switch (pos) {
        case 5: return d->pad.rx;
        case 6: return d->pad.ry;
        case 7: return d->pad.lx;
        case 8: return d->pad.ly;
        default:
            if (d->pressure_enabled && pos >= 9 && pos <= 20)
                return d->pad.pressure[pos - 9];
            return 0x00;
    }
}

void ds2_device_init(ds2_device_t *d) {
    memset(d, 0, sizeof(*d));
    d->analog_mode = true;
    d->mode_locked = true;
    d->pad.buttons_lo = 0xFF;
    d->pad.buttons_hi = 0xFF;
    d->pad.rx = d->pad.ry = d->pad.lx = d->pad.ly = 0x80;
    memset(d->rumble_map, 0xFF, sizeof(d->rumble_map));
    /* Default DS2 reply: digital buttons + four stick axes. */
    d->reply_mask[0] = 0x3F;
    d->reply_mask[1] = 0x00;
    d->reply_mask[2] = 0x00;
    memset(d->button_attr, 2, sizeof(d->button_attr));
}

void ds2_set_pad_state(ds2_device_t *d, const ds2_pad_state_t *state) {
    d->pad = *state;
}

uint8_t ds2_current_id(const ds2_device_t *d) {
    if (d->config_mode) return 0xF3;
    if (!d->analog_mode) return 0x41;
    return d->pressure_enabled ? 0x79 : 0x73;
}

size_t ds2_current_poll_length(const ds2_device_t *d) {
    if (!d->analog_mode) return 5;
    return d->pressure_enabled ? 21 : 9;
}

static uint8_t config_reply_byte(ds2_session_t *s, uint8_t pos) {
    const ds2_device_t *d = s->dev;
    const uint8_t cmd = s->command;
    const uint8_t arg = s->request[3];
    if (pos < 3) return 0x00;

    switch (cmd) {
        case 0x40: { /* DS2 button attribute */
            if (pos == 5) {
                uint8_t idx = s->request[3];
                return idx < 12 ? d->button_attr[idx] : 0xFF;
            }
            return 0x00;
        }
        case 0x41: { /* reply capabilities: 18 data bytes available */
            static const uint8_t r[6] = {0xFF,0xFF,0x03,0x00,0x00,0x00};
            return (pos <= 8) ? r[pos - 3] : 0x00;
        }
        case 0x43: /* config enter/exit */
            if (!s->config_at_start) {
                if (pos == 3) return d->pad.buttons_lo;
                if (pos == 4) return d->pad.buttons_hi;
                if (d->analog_mode) {
                    if (pos == 5) return d->pad.rx;
                    if (pos == 6) return d->pad.ry;
                    if (pos == 7) return d->pad.lx;
                    if (pos == 8) return d->pad.ly;
                }
            }
            return 0x00;
        case 0x44: /* analog/digital + lock */
            if (pos == 5 && s->request[3] > 1) return 0xFF;
            return 0x00;
        case 0x45: { /* DS2 type, constant 02, LED, 02,01,00 */
            uint8_t led = d->analog_mode ? 1 : 0;
            uint8_t r[6] = {0x03,0x02,led,0x02,0x01,0x00};
            return (pos <= 8) ? r[pos - 3] : 0x00;
        }
        case 0x46: {
            static const uint8_t a0[6] = {0x00,0x00,0x01,0x02,0x00,0x0A};
            static const uint8_t a1[6] = {0x00,0x00,0x01,0x01,0x01,0x14};
            const uint8_t *r = arg == 0 ? a0 : (arg == 1 ? a1 : NULL);
            return (r && pos <= 8) ? r[pos - 3] : 0x00;
        }
        case 0x47: {
            static const uint8_t r[6] = {0x00,0x00,0x02,0x00,0x01,0x00};
            return (pos <= 8) ? r[pos - 3] : 0x00;
        }
        case 0x48:
            if (pos == 7 && (arg == 0 || arg == 1)) return 0x01;
            return 0x00;
        case 0x4C: {
            uint8_t r[6] = {0,0,0,0,0,0};
            if (arg == 0) r[3] = 0x04;
            else if (arg == 1) r[3] = 0x07;
            return (pos <= 8) ? r[pos - 3] : 0x00;
        }
        case 0x4D: /* previous rumble mapping */
            return (pos <= 8) ? s->old_rumble_map[pos - 3] : 0x00;
        case 0x4F: /* reply protocol */
            return pos == 8 ? 0x5A : 0x00;
        default:
            return 0x00;
    }
}

static uint8_t next_reply_for_position(ds2_session_t *s, uint8_t pos) {
    ds2_device_t *d = s->dev;
    if (!s->addressed) return 0xFF;
    if (pos == 0) return 0xFF;
    if (pos == 1) return s->config_at_start ? 0xF3 : ds2_current_id(d);
    if (pos == 2) return 0x5A;
    if (s->command == 0x42) return poll_reply_byte(d, pos);
    if (s->command == 0x43 && !s->config_at_start) return config_reply_byte(s, pos);
    if (!s->config_at_start) return 0x00;
    return config_reply_byte(s, pos);
}

void ds2_session_begin(ds2_session_t *s, ds2_device_t *d) {
    memset(s, 0, sizeof(*s));
    s->dev = d;
    s->config_at_start = d->config_mode;
    s->tx_next = 0xFF;
    memcpy(s->old_rumble_map, d->rumble_map, 6);
    memcpy(s->pending_rumble_map, d->rumble_map, 6);
    memcpy(s->pending_reply_mask, d->reply_mask, 6);
}

uint8_t ds2_session_tx(const ds2_session_t *s) { return s->tx_next; }

void ds2_session_rx(ds2_session_t *s, uint8_t rx) {
    ds2_device_t *d = s->dev;
    uint8_t idx = s->byte_index;
    if (idx < sizeof(s->request)) s->request[idx] = rx;

    if (idx == 0) {
        s->addressed = (rx == 0x01);
    } else if (idx == 1 && s->addressed) {
        s->command = rx;
    } else if (s->addressed && idx >= 3) {
        uint8_t data_idx = (uint8_t)(idx - 3);
        switch (s->command) {
            case 0x42:
                if (data_idx < 6) {
                    uint8_t m = d->rumble_map[data_idx];
                    if (m == 0x00) d->rumble_small = (rx == 0xFF) ? 0xFF : 0x00;
                    if (m == 0x01) d->rumble_large = rx;
                }
                break;
            case 0x43:
                if (idx == 3) {
                    s->pending_config_valid = true;
                    s->pending_config = (rx == 0x01);
                }
                break;
            case 0x44:
                if (idx == 3 && rx <= 1) {
                    s->pending_mode_valid = true;
                    s->pending_analog = (rx == 1);
                }
                if (idx == 4) {
                    s->pending_lock_valid = true;
                    s->pending_lock = ((rx & 0x03) == 0x03);
                }
                break;
            case 0x4D:
                if (data_idx < 6) s->pending_rumble_map[data_idx] = rx;
                break;
            case 0x4F:
                if (data_idx < 6) s->pending_reply_mask[data_idx] = rx;
                break;
            case 0x40:
                /* idx 3 = attribute index, idx 4 = new value. */
                if (idx == 4 && s->request[3] < 12)
                    d->button_attr[s->request[3]] = rx & 0x03;
                break;
            default: break;
        }
    }

    s->byte_index++;
    s->tx_next = next_reply_for_position(s, s->byte_index);
}

void ds2_session_end(ds2_session_t *s) {
    if (!s->addressed) return;
    ds2_device_t *d = s->dev;
    if (s->command == 0x4D && s->config_at_start)
        memcpy(d->rumble_map, s->pending_rumble_map, 6);
    if (s->command == 0x4F && s->config_at_start) {
        memcpy(d->reply_mask, s->pending_reply_mask, 6);
        /* The common DS2 all-input mask is FF FF 03. Enable pressure mode
           whenever any of the 12 pressure-response bits is requested. */
        uint32_t mask18 = (uint32_t)d->reply_mask[0] |
                          ((uint32_t)d->reply_mask[1] << 8) |
                          (((uint32_t)d->reply_mask[2] & 0x03u) << 16);
        d->pressure_enabled = (mask18 & 0x3FFC0u) != 0;
    }
    if (s->command == 0x44 && s->config_at_start) {
        if (s->pending_mode_valid) d->analog_mode = s->pending_analog;
        if (s->pending_lock_valid) d->mode_locked = s->pending_lock;
        if (!d->analog_mode) d->pressure_enabled = false;
    }
    if (s->command == 0x43 && s->pending_config_valid)
        d->config_mode = s->pending_config;
}

size_t ds2_exchange_packet(ds2_device_t *d, const uint8_t *cmd, size_t cmd_len,
                           uint8_t *reply, size_t reply_capacity) {
    ds2_session_t s;
    ds2_session_begin(&s, d);
    size_t n = cmd_len < reply_capacity ? cmd_len : reply_capacity;
    for (size_t i = 0; i < n; ++i) {
        reply[i] = ds2_session_tx(&s);
        ds2_session_rx(&s, cmd[i]);
    }
    ds2_session_end(&s);
    return n;
}
