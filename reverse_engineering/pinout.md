# Snapmaker U1 top connector pinout

Connector: 6-pin Molex Micro-Fit 3.0 top-cover/purifier port.

The physical connector numbering below follows the user-provided authoritative diagram.

| Pin | Signal | MCU/config mapping | Verified/use |
|---:|---|---|---|
| 1 | Inner/recirculation PWM | `!PA9` | Active-low approximately 3.23 V PWM, about 997 Hz. External 7530 request input to ESP32. |
| 2 | GND | - | Common low-voltage ground. |
| 3 | Inner-fan tachometer | `PA6` | U1 tach input. Do not apply 24 V. Can be driven only with a level-safe 3.3 V pulse source if tach emulation is required. |
| 4 | FLAG2 / purifier presence sense | `PA7` | Presence resistor: 1 kOhm from Pin 4 to Pin 2 enables the purifier controls. |
| 5 | Exhaust PWM | `!PA8` | Active-high approximately 0-4.8 V PWM, about 997 Hz. Do not connect directly to an ESP32 GPIO without level conversion. |
| 6 | +24 V VCC | - | U1 fused/supplied 24 V rail. Keep separate from ESP32 signal wiring. |

## Bench measurements

Measurements were made relative to Pin 2 with the scope in DC mode and the presence resistor installed.

### Exhaust, Pin 5

| UI slider | Vpp | Vmax | Frequency |
|---:|---:|---:|---:|
| 0% | 0.11 V | approximately 0 V | 0 Hz |
| 50% | 4.99 V | 4.83 V | 997 Hz |
| 100% | 0.15 V | 4.79 V | 0 Hz |

Pin 5 is therefore an active-high variable PWM command with a roughly 5 V signal level and 1 kHz period.

### Recirculation, Pin 1

| UI slider | Vpp | Vmax | Frequency |
|---:|---:|---:|---:|
| 0% | 0.15 V | 3.20 V | 0 Hz |
| 50% | 3.23 V | 3.23 V | 997 Hz |
| 100% | 0.11 V | 0.08 V | 0 Hz |

Pin 1 is therefore an active-low variable PWM command with a roughly 3.3 V signal level and 1 kHz period.

## Software findings

The Snapmaker U1 Klipper configuration uses:

```ini
# exhaust
exhaust_pin: !PA8
exhaust_cycle_time: 0.001
exhaust_hardware_pwm: True

# inner/recirculation
inner_pin: !PA9
inner_cycle_time: 0.001
inner_hardware_pwm: True
inner_tach_pin: PA6
inner_tach_ppr: 2
inner_tach_poll_interval: 0.0005

# presence/power
power_enable_pin: PE15
power_det_pin: PA7
```

The purifier code only raises the inner-fan RPM fault when an inner tachometer object exists and reports zero RPM while the inner fan is commanded. Omitting `inner_tach_pin` disables that check, but also removes inner RPM telemetry.

## Safety

- Never connect Pin 3 to 24 V.
- Never connect Pin 5 directly to an ESP32 input; it reaches approximately 5 V.
- Pin 1 should still be connected through a series resistor during ESP32 testing.
- Keep 110 V AC separate from this low-voltage wiring.
- The ESP32 blower/heater interlock remains independent of any U1 tach emulation.
