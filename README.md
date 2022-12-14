# Minimal Foot Pedal for Spark 40

This project implements a version of a preset switcher for Positive Grid's Spark 40 amplifier
using the [BlueKitchen's BTstack](https://github.com/bluekitchen/btstack).

It is intened to be integrated into basic mechanical foot pedal with up to 4 momentary switches
and uses an ESP32 or variants. 

Current version configured for ESP32-C3 (with RGB LED) with a [DigiTech FS3X 3-Button Footswitch](https://www.digitech.com/foot-controllers/FS3X+3-Button+Footswitch.html) that are mapped to preset 0-2.

Implementation based on [Yury Tsybizov's BLE Message documentation](https://github.com/jrnelson90/tinderboxpedal/blob/master/src/BLE%20message%20format.md).
