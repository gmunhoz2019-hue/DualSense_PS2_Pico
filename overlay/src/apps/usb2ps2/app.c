#include "app.h"

#include "core/input_interface.h"
#include "core/output_interface.h"
#include "core/router/router.h"
#include "core/services/players/manager.h"
#include "core/services/players/feedback.h"
#include "usb/usbh/usbh.h"
#include "native/device/ps2/ps2_device.h"

#include "pico/stdlib.h"
#include <stdio.h>

#define STATUS_LED_PIN 25

static const InputInterface *input_interfaces[] = {
    &usbh_input_interface,
};

static const OutputInterface *output_interfaces[] = {
    &ps2_output_interface,
};

const InputInterface **app_get_input_interfaces(uint8_t *count) {
    *count = (uint8_t)(sizeof(input_interfaces) / sizeof(input_interfaces[0]));
    return input_interfaces;
}

const OutputInterface **app_get_output_interfaces(uint8_t *count) {
    *count = (uint8_t)(sizeof(output_interfaces) / sizeof(output_interfaces[0]));
    return output_interfaces;
}

static void status_led_init(void) {
    gpio_init(STATUS_LED_PIN);
    gpio_set_dir(STATUS_LED_PIN, GPIO_OUT);
    gpio_put(STATUS_LED_PIN, 0);
}

static bool pulse_pattern(uint32_t now, uint8_t pulses) {
    const uint32_t period = 3600u;
    const uint32_t slot = 300u;
    const uint32_t on_ms = 110u;
    uint32_t phase = now % period;
    uint32_t active = (uint32_t)pulses * slot;
    if (phase >= active) return false;
    return (phase % slot) < on_ms;
}

static void status_led_task(void) {
    const uint32_t now = to_ms_since_boot(get_absolute_time());

    if (playersCount <= 0) {
        // No DualSense/player: one pulse every cycle.
        gpio_put(STATUS_LED_PIN, pulse_pattern(now, 1));
        return;
    }

    if (ps2_transaction_count() == 0) {
        // DualSense OK, but no PS2 ATT transaction seen.
        gpio_put(STATUS_LED_PIN, 1);
        return;
    }

    if (ps2_rx_byte_count() == 0) {
        // ATT reaches the Pico, but CLK/CMD never produced a decoded byte.
        gpio_put(STATUS_LED_PIN, pulse_pattern(now, 2));
        return;
    }

    // v7 diagnostic classification of the FIRST command byte from the PS2.
    // Expected first byte is 0x01.
    //
    // 3 pulses = first byte seen as 0xFF (CMD likely high/floating/wrong wire)
    // 4 pulses = first byte seen as 0x00 (CMD likely low/shorted/wrong wire)
    // 5 pulses = first byte seen as 0x80 (classic bit-order/edge symptom)
    // 6 pulses = another unexpected first-byte value
    // 7 pulses = valid 0x01 seen, but no 0x42 command yet
    // 8 pulses = valid 0x01 0x42 poll decoded
    uint8_t pulses = 6;

    if (ps2_poll42_count() > 0 && ps2_address_count() > 0) {
        pulses = 8;
    } else if (ps2_address_count() > 0) {
        pulses = 7;
    } else if (ps2_first_80_count() > 0) {
        pulses = 5;
    } else if (ps2_first_ff_count() > 0) {
        pulses = 3;
    } else if (ps2_first_00_count() > 0) {
        pulses = 4;
    } else {
        pulses = 6;
    }

    gpio_put(STATUS_LED_PIN, pulse_pattern(now, pulses));
}

void app_init(void) {
    router_config_t cfg = {
        .mode = ROUTING_MODE_MERGE,
        .merge_mode = MERGE_BLEND,
        .max_players_per_output = {
            [OUTPUT_TARGET_PS2] = 1,
        },
        .merge_all_inputs = true,
        .transform_flags = TRANSFORM_NONE,
        .mouse_drain_rate = 0,
        .mouse_target_x = ANALOG_LX,
        .mouse_target_y = ANALOG_LY,
    };
    router_init(&cfg);

    // Wired-only build: DualSense USB HID -> virtual DualShock 2.
    router_add_route(INPUT_SOURCE_USB_HOST, OUTPUT_TARGET_PS2, 0);

    player_config_t players_cfg = {
        .slot_mode = PLAYER_SLOT_FIXED,
        .max_slots = 1,
        .auto_assign_on_press = true,
    };
    players_init_with_config(&players_cfg);

    status_led_init();
    printf("[usb2ps2-wired] GP0=D+, GP1=D- DualSense USB -> GP6..10 PS2 output\n");
}

void app_task(void) {
    static uint8_t last_large = 0;
    static uint8_t last_small = 0;

    status_led_task();

    uint8_t large = ps2_rumble_large();
    uint8_t small = ps2_rumble_small();

    if (playersCount <= 0) {
        last_large = large;
        last_small = small;
        return;
    }

    if (large != last_large || small != last_small) {
        feedback_set_rumble(0, large, small);
        last_large = large;
        last_small = small;
    }
}
