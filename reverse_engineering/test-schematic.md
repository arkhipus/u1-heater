# Temporary ESP32 PWM observation schematic

This is a bench-only signal observation setup. It does not connect either external fan or the heater.

```text
                       Snapmaker U1 top connector

             Pin 1 PA9 (recirculation, 0..3.3 V PWM)
                         |
                       10 kOhm
                         |
                         +---------------- ESP32 GPIO 27

             Pin 5 PA8 (exhaust, approximately 0..5 V PWM)
                         |
                       15 kOhm
                         |
                         +---------------- ESP32 GPIO 26
                         |
                       22 kOhm
                         |
             Pin 2 GND --+---------------- ESP32 GND

             Pin 4 FLAG2 --- 1 kOhm --- Pin 2 GND
             Pin 6 +24 V:   NOT CONNECTED
             Pin 3 PA6:     NOT CONNECTED for this test
```

The Pin 5 divider gives approximately 2.97 V at the ESP32 input when Pin 5 is 5 V:

```text
Vgpio = 5 V * 22 kOhm / (15 kOhm + 22 kOhm) = 2.97 V
```

The Pin 1 10 kOhm resistor is a series protection resistor. Pin 1 has been measured at approximately 3.23 V maximum. Do not use this wiring until that level has been confirmed with the presence resistor installed.

## Test procedure

1. Keep the 1 kOhm presence resistor installed.
2. Power the ESP32 from USB only.
3. Connect U1 Pin 2 to ESP32 GND.
4. Connect Pin 1 through 10 kOhm to GPIO 27.
5. Connect Pin 5 through the 15 kOhm/22 kOhm divider to GPIO 26.
6. Leave U1 Pin 6, Pin 3, GPIO 23, the 7530, and the heater disconnected.
7. Flash `pwm_input_test.ino` and open the serial monitor at 115200 baud.
8. Move each U1 slider and check the reported frequency, duty, and inferred state.

Pin 1 is active-low, so the sketch inverts its measured duty. Pin 5 is active-high.

## Optional tach test, separate from this schematic

If tach monitoring remains enabled, use a separately tested 3.3 V pulse source:

```text
ESP32 output -- 1 kOhm -- U1 Pin 3 (PA6)
ESP32 GND ------------- U1 Pin 2
```

Use pulses, not a constant HIGH level. Start at 100 Hz, 50% duty. With `inner_tach_ppr: 2`, U1 interprets that as approximately 1500 RPM. Never use a 24 V pull-up.
