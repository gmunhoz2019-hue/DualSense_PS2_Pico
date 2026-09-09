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

static void status_led_task(void) {
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    static uint32_t last_ps2_count = 0;
    static uint32_t last_ps2_seen_ms = 0;

    const uint32_t ps2_count = ps2_transaction_count();
    if (ps2_count != last_ps2_count) {
        last_ps2_count = ps2_count;
        last_ps2_seen_ms = now;
    }

    bool on = false;

    if (playersCount <= 0) {
        // No DualSense/player yet: one short pulse every 2 s.
        const uint32_t phase = now % 2000u;
        on = (phase < 120u);
    } else if ((now - last_ps2_seen_ms) < 500u) {
        // DualSense is recognized AND the PS2 ATT line is actively producing
        // transactions. Fast blink means both halves of the adapter are alive.
        on = ((now / 100u) & 1u) != 0u;
    } else {
        // DualSense recognized, but no PS2 transaction seen recently.
        // Solid LED isolates the fault to the PS2 wiring/ATT/transport side.
        on = true;
    }

    gpio_put(STATUS_LED_PIN, on);
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
