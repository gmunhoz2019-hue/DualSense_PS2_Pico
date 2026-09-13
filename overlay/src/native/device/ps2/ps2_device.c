#include "ps2_device.h"
#include "ds2_protocol.h"
#include "ps2_bus.pio.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/irq.h"

#include "core/buttons.h"
#include "core/router/router.h"
#include "core/services/players/manager.h"

#include <string.h>

// PIO1 is occupied by Pico-PIO-USB on this non-CYW43 RP2040 build.
// Keep the PS2 controller bus isolated on PIO0.
static PIO const g_pio = pio0;
static uint g_sm_rx;
static uint g_sm_tx;
static uint g_off_rx;
static uint g_off_tx;

static ds2_device_t g_ds2;
static ds2_pad_state_t g_pad_buf[2];
static volatile uint8_t g_pad_active = 0;
static volatile uint32_t g_att_epoch = 0;
static volatile uint32_t g_rx_byte_count = 0;
static volatile uint32_t g_address_count = 0;
static volatile uint32_t g_poll42_count = 0;
static volatile uint32_t g_first_ff_count = 0;
static volatile uint32_t g_first_00_count = 0;
static volatile uint32_t g_first_80_count = 0;
static volatile uint32_t g_first_other_count = 0;
static volatile uint8_t g_last_first_byte = 0xFF;
static volatile uint8_t g_last_second_byte = 0xFF;

static inline void set_active_low(uint8_t *v, uint8_t mask, bool pressed) {
    if (pressed) *v &= (uint8_t)~mask;
}

static ds2_pad_state_t neutral_pad(void) {
    ds2_pad_state_t s;
    memset(&s, 0, sizeof(s));
    s.buttons_lo = 0xFF;
    s.buttons_hi = 0xFF;
    s.rx = s.ry = s.lx = s.ly = 0x80;
    return s;
}

static void event_to_ds2(const input_event_t *e, ds2_pad_state_t *s) {
    *s = neutral_pad();
    if (!e) return;

    const uint32_t b = e->buttons;

    // Byte 1: Select, L3, R3, Start, Up, Right, Down, Left (active-low)
    set_active_low(&s->buttons_lo, 0x01, (b & JP_BUTTON_S1) != 0);
    set_active_low(&s->buttons_lo, 0x02, (b & JP_BUTTON_L3) != 0);
    set_active_low(&s->buttons_lo, 0x04, (b & JP_BUTTON_R3) != 0);
    set_active_low(&s->buttons_lo, 0x08, (b & JP_BUTTON_S2) != 0);
    set_active_low(&s->buttons_lo, 0x10, (b & JP_BUTTON_DU) != 0);
    set_active_low(&s->buttons_lo, 0x20, (b & JP_BUTTON_DR) != 0);
    set_active_low(&s->buttons_lo, 0x40, (b & JP_BUTTON_DD) != 0);
    set_active_low(&s->buttons_lo, 0x80, (b & JP_BUTTON_DL) != 0);

    // Byte 2: L2, R2, L1, R1, Triangle, Circle, Cross, Square (active-low)
    set_active_low(&s->buttons_hi, 0x01, (b & JP_BUTTON_L2) != 0);
    set_active_low(&s->buttons_hi, 0x02, (b & JP_BUTTON_R2) != 0);
    set_active_low(&s->buttons_hi, 0x04, (b & JP_BUTTON_L1) != 0);
    set_active_low(&s->buttons_hi, 0x08, (b & JP_BUTTON_R1) != 0);
    set_active_low(&s->buttons_hi, 0x10, (b & JP_BUTTON_B4) != 0); // Triangle
    set_active_low(&s->buttons_hi, 0x20, (b & JP_BUTTON_B2) != 0); // Circle
    set_active_low(&s->buttons_hi, 0x40, (b & JP_BUTTON_B1) != 0); // Cross
    set_active_low(&s->buttons_hi, 0x80, (b & JP_BUTTON_B3) != 0); // Square

    s->lx = e->analog[ANALOG_LX];
    s->ly = e->analog[ANALOG_LY];
    s->rx = e->analog[ANALOG_RX];
    s->ry = e->analog[ANALOG_RY];

    // DS2 pressure response order:
    // Right, Left, Up, Down, Triangle, Circle, Cross, Square, L1, R1, L2, R2.
    if (e->has_pressure) {
        // Joypad pressure input order:
        // Up, Right, Down, Left, L2, R2, L1, R1, Triangle, Circle, Cross, Square.
        s->pressure[0]  = e->pressure[1];
        s->pressure[1]  = e->pressure[3];
        s->pressure[2]  = e->pressure[0];
        s->pressure[3]  = e->pressure[2];
        s->pressure[4]  = e->pressure[8];
        s->pressure[5]  = e->pressure[9];
        s->pressure[6]  = e->pressure[10];
        s->pressure[7]  = e->pressure[11];
        s->pressure[8]  = e->pressure[6];
        s->pressure[9]  = e->pressure[7];
        s->pressure[10] = e->pressure[4];
        s->pressure[11] = e->pressure[5];
    } else {
        s->pressure[0] = (b & JP_BUTTON_DR) ? 0xFF : 0;
        s->pressure[1] = (b & JP_BUTTON_DL) ? 0xFF : 0;
        s->pressure[2] = (b & JP_BUTTON_DU) ? 0xFF : 0;
        s->pressure[3] = (b & JP_BUTTON_DD) ? 0xFF : 0;
        s->pressure[4] = (b & JP_BUTTON_B4) ? 0xFF : 0;
        s->pressure[5] = (b & JP_BUTTON_B2) ? 0xFF : 0;
        s->pressure[6] = (b & JP_BUTTON_B1) ? 0xFF : 0;
        s->pressure[7] = (b & JP_BUTTON_B3) ? 0xFF : 0;
        s->pressure[8] = (b & JP_BUTTON_L1) ? 0xFF : 0;
        s->pressure[9] = (b & JP_BUTTON_R1) ? 0xFF : 0;
        s->pressure[10] = e->analog[ANALOG_L2];
        s->pressure[11] = e->analog[ANALOG_R2];
    }
}

static void publish_pad(const ds2_pad_state_t *s) {
    uint8_t next = (uint8_t)(g_pad_active ^ 1u);
    g_pad_buf[next] = *s;
    __dmb();
    g_pad_active = next;
}

static void ps2_att_irq(void) {
    uint32_t events = gpio_get_irq_event_mask(PS2_PIN_ATT);
    if (events & GPIO_IRQ_EDGE_RISE) {
        gpio_acknowledge_irq(PS2_PIN_ATT, GPIO_IRQ_EDGE_RISE);
        g_att_epoch++;
    }
}

static void ps2_init(void) {
    ds2_device_init(&g_ds2);
    g_pad_buf[0] = neutral_pad();
    g_pad_buf[1] = g_pad_buf[0];

    // Console-driven inputs.
    gpio_init(PS2_PIN_CLK);
    gpio_init(PS2_PIN_ATT);
    gpio_init(PS2_PIN_CMD);
    gpio_set_dir(PS2_PIN_CLK, GPIO_IN);
    gpio_set_dir(PS2_PIN_ATT, GPIO_IN);
    gpio_set_dir(PS2_PIN_CMD, GPIO_IN);
    gpio_disable_pulls(PS2_PIN_CLK);
    gpio_disable_pulls(PS2_PIN_ATT);
    gpio_disable_pulls(PS2_PIN_CMD);

    // Controller-driven lines are open-drain and initialized by the PIO helper.
    gpio_init(PS2_PIN_DATA);
    gpio_init(PS2_PIN_ACK);
    gpio_disable_pulls(PS2_PIN_DATA);
    gpio_disable_pulls(PS2_PIN_ACK);

    g_sm_rx = pio_claim_unused_sm(g_pio, true);
    g_sm_tx = pio_claim_unused_sm(g_pio, true);
    g_off_rx = pio_add_program(g_pio, &ps2_cmd_reader_program);
    g_off_tx = pio_add_program(g_pio, &ps2_dat_writer_program);
    ps2_cmd_reader_program_init(g_pio, g_sm_rx, g_off_rx);
    ps2_dat_writer_program_init(g_pio, g_sm_tx, g_off_tx);

    // Track ATT rising edges as transaction boundaries. Shared IRQ handler keeps
    // this compatible with Joypad services that may also use IO_IRQ_BANK0.
    gpio_set_irq_enabled(PS2_PIN_ATT, GPIO_IRQ_EDGE_RISE, true);
    irq_add_shared_handler(IO_IRQ_BANK0, ps2_att_irq,
                           PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(IO_IRQ_BANK0, true);

    pio_enable_sm_mask_in_sync(g_pio, (1u << g_sm_rx) | (1u << g_sm_tx));
}

static void ps2_task(void) {
    const input_event_t *e = router_get_output(OUTPUT_TARGET_PS2, 0);
    ds2_pad_state_t s;
    if (e && playersCount > 0) event_to_ds2(e, &s);
    else s = neutral_pad();
    publish_pad(&s);
}

static void ps2_core1_task(void) {
    ds2_session_t session;
    bool have_session = false;
    uint32_t seen_epoch = g_att_epoch;

    while (true) {
        uint8_t rx = ps2_read_byte_blocking(g_pio, g_sm_rx);
        uint32_t now = g_att_epoch;

        if (!have_session || now != seen_epoch) {
            if (have_session) ds2_session_end(&session);

            // Snapshot the newest controller state once per PS2 transaction.
            uint8_t idx = g_pad_active;
            __dmb();
            ds2_pad_state_t snapshot = g_pad_buf[idx];
            ds2_set_pad_state(&g_ds2, &snapshot);

            ds2_session_begin(&session, &g_ds2);
            seen_epoch = now;
            have_session = true;
        }

        // Diagnostic counters let the LED distinguish ATT-only activity from
        // actual decoded CLK/CMD bytes and a real 0x01 0x42 controller poll.
        uint8_t byte_index = session.byte_index;
        g_rx_byte_count++;
        if (byte_index == 0) {
            g_last_first_byte = rx;
            if (rx == 0x01) {
                g_address_count++;
            } else if (rx == 0xFF) {
                g_first_ff_count++;
            } else if (rx == 0x00) {
                g_first_00_count++;
            } else if (rx == 0x80) {
                g_first_80_count++;
            } else {
                g_first_other_count++;
            }
        }
        if (byte_index == 1) {
            g_last_second_byte = rx;
            if (rx == 0x42) g_poll42_count++;
        }

        ds2_session_rx(&session, rx);

        // Response to the NEXT command byte. DATA remains released for the first
        // byte (0xFF); after that, queuing also causes the writer SM to pulse ACK.
        ps2_write_byte_blocking(g_pio, g_sm_tx, ds2_session_tx(&session));
    }
}

uint8_t ps2_rumble_large(void) { return g_ds2.rumble_large; }
uint8_t ps2_rumble_small(void) { return g_ds2.rumble_small; }
uint32_t ps2_transaction_count(void) { return g_att_epoch; }
uint32_t ps2_rx_byte_count(void) { return g_rx_byte_count; }
uint32_t ps2_address_count(void) { return g_address_count; }
uint32_t ps2_poll42_count(void) { return g_poll42_count; }
uint32_t ps2_first_ff_count(void) { return g_first_ff_count; }
uint32_t ps2_first_00_count(void) { return g_first_00_count; }
uint32_t ps2_first_80_count(void) { return g_first_80_count; }
uint32_t ps2_first_other_count(void) { return g_first_other_count; }
uint8_t ps2_last_first_byte(void) { return g_last_first_byte; }
uint8_t ps2_last_second_byte(void) { return g_last_second_byte; }

const OutputInterface ps2_output_interface = {
    .name = "PlayStation 2 DualShock 2",
    .target = OUTPUT_TARGET_PS2,
    .init = ps2_init,
    .task = ps2_task,
    .core1_task = ps2_core1_task,
    .get_feedback = NULL,
    .get_rumble = ps2_rumble_large,
    .get_player_led = NULL,
    .get_profile_count = NULL,
    .get_active_profile = NULL,
    .set_active_profile = NULL,
    .get_profile_name = NULL,
    .get_trigger_threshold = NULL,
    .get_native_config = NULL,
    .set_native_config = NULL,
};
