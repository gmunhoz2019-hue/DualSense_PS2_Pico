#!/usr/bin/env python3
from pathlib import Path
import shutil
import sys

root = Path(sys.argv[1]).resolve()
overlay = Path(__file__).resolve().parents[1] / "overlay"

if not (root / "src/CMakeLists.txt").exists():
    raise SystemExit(f"Not a Joypad OS tree: {root}")

# Copy our app/device implementation into the pinned Joypad OS checkout.
for src in overlay.rglob("*"):
    if src.is_dir():
        continue
    rel = src.relative_to(overlay)
    dst = root / rel
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)

# Add a dedicated router output target.
router_h = root / "src/core/router/router.h"
text = router_h.read_text()
needle = "    OUTPUT_TARGET_REMOTE_PLAY,      // PS Remote Play (WiFi) — usb2wifi app\n    OUTPUT_TARGET_COUNT"
repl = "    OUTPUT_TARGET_REMOTE_PLAY,      // PS Remote Play (WiFi) — usb2wifi app\n    OUTPUT_TARGET_PS2,              // PlayStation 2 DualShock 2 device output\n    OUTPUT_TARGET_COUNT"
if needle not in text:
    if "OUTPUT_TARGET_PS2" not in text:
        raise SystemExit("Could not patch output_target_t in router.h")
else:
    router_h.write_text(text.replace(needle, repl, 1))

# Add the dedicated Raspberry Pi Pico target. PIO-USB uses PIO1 on non-CYW43
# builds; our PS2 device driver explicitly uses PIO0.
cmake = root / "src/CMakeLists.txt"
text = cmake.read_text()
marker = "# === DualSense_PS2_Pico target ==="
block = r'''

# === DualSense_PS2_Pico target ===
# Raspberry Pi Pico RP2040: PIO USB host on GP0/GP1 + PS2 device on GP6..GP10.
add_executable(joypad_ps2_pico)
target_compile_definitions(joypad_ps2_pico PRIVATE
    CONFIG_USB=1
    PICO_DEFAULT_PIO_USB_DP_PIN=0
    BOARD_TUH_RHPORT=1
    CONFIG_NO_NEOPIXEL=1
    USE_BOOTSEL_BUTTON=1
    BOARD_LED_PIN=25
    MAX_PLAYERS=1
    MAX_PLAYERS_PER_OUTPUT=1
)
target_sources(joypad_ps2_pico PUBLIC
    ${COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/ps2/ds2_protocol.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/ps2/ps2_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2ps2/app.c
)
target_include_directories(joypad_ps2_pico PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2ps2
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/ps2
)
target_link_libraries(joypad_ps2_pico PRIVATE
    ${COMMON_LIBRARIES}
    tinyusb_pico_pio_usb
    hardware_flash
)
joypad_target_common(joypad_ps2_pico)
joypad_add_btstack(joypad_ps2_pico)
pico_generate_pio_header(joypad_ps2_pico
    ${CMAKE_CURRENT_LIST_DIR}/native/device/ps2/ps2_bus.pio)
pico_enable_stdio_usb(joypad_ps2_pico 0)
pico_enable_stdio_uart(joypad_ps2_pico 0)
# === end DualSense_PS2_Pico target ===
'''
if marker not in text:
    cmake.write_text(text + block)

print("Prepared Joypad OS tree for DualSense_PS2_Pico")
