#include "app.h"

#include "core/input_interface.h"
#include "core/output_interface.h"
#include "core/router/router.h"
#include "core/services/players/manager.h"
#include "core/services/players/feedback.h"
#include "usb/usbh/usbh.h"
#include "native/device/ps2/ps2_device.h"
#include "bt/btstack/btstack_host.h"

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
    const uint8_t usb_count = usbh_get_device_count();
    const bool hci_ready = btstack_host_is_powered_on();
    const uint8_t classic_count = btstack_classic_get_connection_count();
    bool on = false;

    if (playersCount > 0 || classic_count > 0) {
        // Controller link exists / player assigned: solid ON.
        on = true;
    } else if (usb_count == 0) {
        // No USB device enumerated on GP0/GP1: one short pulse every 2 seconds.
        const uint32_t phase = now % 2000u;
        on = (phase < 120u);
    } else if (!hci_ready) {
        // USB device mounted, but Bluetooth HCI is not working yet: two pulses.
        const uint32_t phase = now % 2000u;
        on = (phase < 120u) || (phase >= 300u && phase < 420u);
    } else {
        // Bluetooth controller is powered/scanning and waiting for a gamepad:
        // rapid blink, 200 ms per state.
        on = ((now / 200u) & 1u) != 0u;
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

    // Wired USB controllers and Bluetooth HID controllers both feed the same
    // virtual DualShock 2. BT HID drivers submit as BLE_CENTRAL for both
    // Classic HID and BLE/HOGP in Joypad's unified routing layer.
    router_add_route(INPUT_SOURCE_USB_HOST, OUTPUT_TARGET_PS2, 0);
    router_add_route(INPUT_SOURCE_BLE_CENTRAL, OUTPUT_TARGET_PS2, 0);

    player_config_t players_cfg = {
        .slot_mode = PLAYER_SLOT_FIXED,
        .max_slots = 1,
        .auto_assign_on_press = 0,
    };
    players_init_with_config(&players_cfg);

    status_led_init();

    printf("[usb2ps2] GP0/1 USB Bluetooth host -> GP6..10 PS2 output\n");
}

void app_task(void) {
    static uint8_t last_large = 0;
    static uint8_t last_small = 0;
    static uint32_t last_scan_request_ms = 0;

    const uint32_t now = to_ms_since_boot(get_absolute_time());
    const bool hci_ready = btstack_host_is_powered_on();
    const uint8_t classic_count = btstack_classic_get_connection_count();

    // The first hardware test showed the Pico alive but the DualSense stayed in
    // pairing mode. Make discovery explicit instead of relying only on the host's
    // automatic scan/restart state machine. Retry every 3 s only while no BT link
    // exists, so we do not interfere with an in-progress/established DS5 link.
    if (hci_ready && playersCount <= 0 && classic_count == 0) {
        if (!btstack_host_is_scanning() && (now - last_scan_request_ms >= 3000u)) {
            btstack_host_suppress_scan(false);
            btstack_host_start_scan();
            last_scan_request_ms = now;
        }
    }

    status_led_task();

    uint8_t large = ps2_rumble_large();
    uint8_t small = ps2_rumble_small();

    if (playersCount <= 0) {
        last_large = large;
        last_small = small;
        return;
    }

    if (large != last_large || small != last_small) {
        // PS2 small motor is on/off; expose it as the high-frequency/right motor.
        feedback_set_rumble(0, large, small);
        last_large = large;
        last_small = small;
    }
}
