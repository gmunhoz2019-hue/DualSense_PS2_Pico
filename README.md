# DualSense PS2 Pico

Firmware project for a Raspberry Pi Pico (RP2040) that accepts a DualSense over a USB Bluetooth dongle and emulates a PlayStation 2 DualShock 2 controller.

## Fixed wiring for this build

### USB Bluetooth host (PIO USB)
- GP0 = USB D+
- GP1 = USB D-
- GND = USB GND
- VBUS = USB +5 V

### PlayStation 2 controller bus
- GP6 = CLK (PS2 -> Pico)
- GP7 = ATT / CS (PS2 -> Pico)
- GP8 = CMD / DI (PS2 -> Pico)
- GP9 = DATA / DO (Pico -> PS2, open-drain)
- GP10 = ACK (Pico -> PS2, open-drain)
- GND = common ground

PS2 3.3 V and 7.5 V remain disconnected while the Pico is powered from USB during development.

## Bluetooth target
The initial hardware target is a USB Bluetooth adapter reporting VID:PID `0A12:0001`, including the common `bcdDevice 0x8891` clone family.

The build is based on Joypad OS for USB-host/Bluetooth/DualSense support, with a custom PS2 device output using RP2040 PIO.
