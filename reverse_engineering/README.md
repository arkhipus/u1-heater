# U1 top-connector reverse engineering

This folder records the verified Snapmaker U1 top-cover connector mapping, bench measurements, and the temporary ESP32 input test.

## Scope

The goal is to substitute the top-cover fan functions:

- U1 recirculation/inner-fan command drives the external 7530 heater blower through ESP32 logic and a MOSFET.
- U1 exhaust command drives the external 120 mm PWM Noctua.
- U1's inner-fan tach check may be disabled by omitting `inner_tach_pin`, or tested with a safe 3.3 V pulse source on Pin 3.

## Contents

- `pinout.md` - finalized connector mapping and measurements.
- `test-schematic.md` - temporary two-input ESP32 bench wiring.
- `pwm_input_test.ino` - standalone Arduino sketch that reports both inferred fan requests.

This is bench/reverse-engineering documentation. Do not connect unknown signals to GPIOs or apply 24 V to signal pins.
