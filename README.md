# Snapmaker U1 Chamber Controller - U1-driven design

This branch defines requirements for an ESP32 companion controller for the Snapmaker U1 chamber heater, recirculation path, and outdoor exhaust.

**Branch:** `u1-driven-design`

**Phase:** requirements and validation only

**Firmware:** intentionally not being changed yet

**Hardware schematic:** intentionally unchanged

## Design direction

The Snapmaker U1 becomes the source of operating intent. Its UI and filament profiles already expose purifier/chamber behavior through Klipper and Moonraker. The ESP32 translates that intent into safe control of the external hardware.

```text
U1 UI / filament profile
          │
          ▼
   Moonraker purifier telemetry
          │
          ▼
  ESP32 arbitration and safety layer
          │
          ├── binary recirculation blower
          ├── binary PTC heater
          ├── butterfly-valve servo
          └── PWM-capable outdoor exhaust fan
```

The ESP32 remains the safety authority. U1 commands are requests, not guarantees of heater safety.

## Five-position controller

The existing five-position switch is retained and redefined as:

| Position | Mode | Function |
|---|---|---|
| 1 | `OFF` | Force heater, blower, and exhaust off. |
| 2 | `AUTO` | Follow U1 UI and filament-profile intent. |
| 3 | `PREHEAT` | Locally heat the chamber to the configured target. |
| 4 | `PURGE` | Open the valve and exhaust; heater off. |
| 5 | `SERVICE` | Test valve and fans with the heater hard-disabled. |

## Actuator facts

- The external recirculation blower is **on/off only** and has no tachometer.
- The PTC heater is **on/off only** and is controlled thermostatically.
- The outdoor Noctua fan has PWM and should track a validated U1 exhaust percentage.
- The butterfly flap is servo-controlled but automatic operation uses fixed closed/open endpoints; U1 currently appears to treat exhaust as on/off.
- The external heater permission remains protected by the blower relay interlock, KSD301 thermostat, and one-shot thermal fuse.
- Exhaust is never allowed against a closed valve.

The U1's native purifier fan RPM must not be treated as proof that the external blower is operating.

## Safety baseline

The heater must default OFF on boot, invalid or stale telemetry, invalid switch state, over-temperature, purge/exhaust operation, blower command OFF, loss of hardware heater permission, or internal fault.

The hardware cutoffs are independent emergency protection. They are not a substitute for correct firmware behavior.

## Documentation

- [`docs/requirements.md`](docs/requirements.md) - authoritative requirements and verification plan
- [`docs/esp32_chamber_controller_notes.md`](docs/esp32_chamber_controller_notes.md) - engineering reference and design constraints
- [`docs/ESP32_review.md`](docs/ESP32_review.md) - requirements-phase review and unresolved decisions
- [`docs/chamber_controller_schematic.drawio`](docs/chamber_controller_schematic.drawio) - unchanged hardware schematic
- [`arduino/esp32_heater.cpp/esp32_heater.cpp.ino`](arduino/esp32_heater.cpp/esp32_heater.cpp.ino) - existing prototype, intentionally out of scope until requirements are approved

## Requirements-first workflow

1. Capture U1 purifier telemetry for UI and filament-profile combinations.
2. Resolve command units, mode semantics, timeout policy, and arbitration.
3. Review and freeze safety requirements.
4. Test the assembled controller with the heater disconnected.
5. Implement firmware only after the above are signed off.
6. Perform electrical and mains testing last.

No Git commit or remote push is made without Alex's explicit approval.
