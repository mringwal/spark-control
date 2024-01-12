# Minimal Foot Pedal for Spark 40

This project implements a version of a preset switcher for Positive Grid's Spark 40 amplifier
using the [BlueKitchen's BTstack](https://github.com/bluekitchen/btstack).

It is intened to be integrated into basic mechanical foot pedal with up to 4 momentary switches and uses an ESP32 or variants. 

Current version configured for ESP32 with a [DigiTech FS3X 3-Button Footswitch](https://www.digitech.com/foot-controllers/FS3X+3-Button+Footswitch.html) that are mapped to preset 0-2. The FS3X has a 3-pin 6.5mm headphone connector with one GND and two signal lines. MODE button connects the first line to GND. DOWN button connects the second line to GND. The UP button connects both signal lines with 2 diodes to GND. To simplify the logic, the diodes
have been removed and the buttons have been connected directly.

3 WS2812b ("NeoPixel") LEDs are used to indicate the current preset and the connection state.

Implementation based on [Yury Tsybizov's BLE Message documentation](https://github.com/jrnelson90/tinderboxpedal/blob/master/src/BLE%20message%20format.md).

GPIO | Function  | Notes
-----|-----------|------
0    | LED strip | 
18   | Button A  |
19   | Button B  |  
21   | Button C  |  
