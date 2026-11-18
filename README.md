# Snapmaker U1 Chamber Controller - U1-driven design

This branch defines requirements for an ESP32 companion controller for the Snapmaker U1 chamber heater, recirculation path, and outdoor exhaust.

**Branch:** `u1-driven-design`

**Phase:** staged firmware implementation and low-voltage validation

**Firmware:** initial U1-driven implementation in progress

**Hardware reference:** verified as-built schematic

## Design direction

The Snapmaker U1 becomes the source of operating intent. Its UI and filament profiles already expose purifier/chamber behavior through Klipper and Moonraker. The ESP32 translates that intent into safe control of the external hardware.

```text
U1 UI / filament profile
          │
          ▼
   Moonraker purifier telemetry
          │
          ▼
  ESP32 safety and actuator layer
          │
          ├── binary recirculation blower
          ├── binary PTC heater
          └── butterfly-valve servo, following observed exhaust PWM

U1 Pin 5 exhaust PWM ───────────────► Noctua exhaust fan
                 └── GPIO 26 monitor ─► ESP32
```

The ESP32 remains the safety authority. U1 commands are requests, not guarantees of heater safety.

## Actuator facts

- The external recirculation blower is **on/off only** and has no tachometer: `purifier.inner_fan.speed > 0` turns it ON; zero turns it OFF. Never use the U1 inner-fan RPM as external-blower feedback.
- The PTC heater is **on/off only** and is controlled thermostatically.
- GPIO 25 drives the replacement 3.3 V-compatible, active-high heater relay module (HIGH = ON, LOW = OFF). The module power connections and switched heater contacts follow the verified as-built schematic; confirm the contact rating against heater current before mains operation.
- U1 Pin 5 directly supplies PWM to the outdoor Noctua fan; GPIO 26 observes that signal.
- GPIO 27 observes the U1 recirculation request. GPIO 19 is not connected to the Noctua PWM input.
- The U1 onboard 24 V supply powers the assembly; Pin 6 supplies the Noctua fan only.
- The butterfly flap is servo-controlled but automatic operation uses fixed closed/open endpoints; U1 currently appears to treat exhaust as on/off.
- The external heater permission remains protected by the blower relay interlock, KSD301 thermostat, and one-shot thermal fuse.
- The ESP32 commands the flap open while exhaust PWM is active and closed when it is inactive.

The U1's native purifier fan RPM must not be treated as proof that the external blower is operating.

## Safety baseline

The heater must default OFF on boot, missing or invalid safety-critical data, an unsafe mode-specific exhaust/heating combination, blower command OFF, or internal fault. The KSD301 thermostat and one-shot thermal fuse provide independent over-temperature protection; firmware does not add a separate software cutoff. The heater uses a 2 °C hysteresis band, with no extra minimum on/off dwell during initial debugging. After valid telemetry, a transport failure holds the last heater command for a configurable 60-second grace, then turns heat OFF and displays `Telemetry lost`.

The hardware cutoffs are independent emergency protection. They are not a substitute for correct firmware behavior.

## Documentation

- [`docs/requirements.md`](docs/requirements.md) - authoritative requirements and verification plan
- [`docs/esp32_chamber_controller_notes.md`](docs/esp32_chamber_controller_notes.md) - engineering reference and design constraints
- [`docs/chamber_controller_schematic.drawio`](docs/chamber_controller_schematic.drawio) - verified hardware schematic
- [`docs/chamber_controller_schematic-Copy of Page-2.pdf`](docs/chamber_controller_schematic-Copy%20of%20Page-2.pdf) - verified as-built schematic
- [`arduino/esp32_heater.cpp/esp32_heater.cpp.ino`](arduino/esp32_heater.cpp/esp32_heater.cpp.ino) - staged U1-driven firmware implementation

## Staged implementation workflow

1. Use the approved frozen baseline and checked-in telemetry examples for the initial firmware.
2. Fail the heater OFF for invalid, missing, or unsupported safety-critical telemetry.
3. Validate low-voltage behavior with the heater disconnected and capture live U1 telemetry.
4. Install/connect the heater only after low-voltage behavior is verified.
5. Perform mains testing last, after electrical inspection and explicit approval.

No Git commit or remote push is made without Alex's explicit approval.
