# AGENTS.md - U1-driven design branch

This repository defines an ESP32 companion controller for the Snapmaker U1 chamber heater, recirculation path, and outdoor exhaust.

## Current phase

The `u1-driven-design` branch is requirements-first. Do not implement firmware, modify the electrical schematic, or perform mains testing until the requirements in `docs/requirements.md` are reviewed and frozen.

The existing Arduino sketch is historical prototype material. It is intentionally not the design authority and should not be used as a reference implementation while requirements are being rewritten.

## Authoritative documents

- `docs/requirements.md` - authoritative requirements and verification plan.
- `docs/esp32_chamber_controller_notes.md` - engineering reference.
- `docs/ESP32_review.md` - requirements-phase review and unresolved decisions.
- `README.md` - project overview.
- `docs/chamber_controller_schematic.drawio` - current hardware schematic, unchanged in this phase.

## Design constraints

- U1 UI and filament profiles provide operating intent through Moonraker purifier telemetry.
- ESP32 provides arbitration, safety, and external actuator control.
- External recirculation blower is binary on/off and has no tachometer.
- PTC heater is binary on/off and thermostatically controlled.
- Outdoor exhaust PWM should track a validated U1 exhaust percentage when available.
- The U1 exhaust request appears to be on/off for the butterfly flap: automatic operation uses fixed closed/open servo endpoints, not partial flap angles.
- Outdoor exhaust must never run with the butterfly valve closed.
- Heater defaults OFF on boot, reset, invalid/stale telemetry, invalid switch state, purge/exhaust, over-temperature, blower command OFF, interlock loss, or internal fault.
- Existing blower-powered heater interlock, KSD301 thermostat, and one-shot thermal fuse remain mandatory.
- Do not claim external-blower airflow or RPM verification without adding a sensor.
- Keep 110 V AC separate from low-voltage wiring.
- Never connect unverified U1 signals directly to ESP32 GPIO.

## Five-position switch

The requirements-level mapping is:

```text
OFF / AUTO / PREHEAT / PURGE / SERVICE
```

`AUTO` follows U1 intent. `PREHEAT` is local closed-loop heating. `PURGE` is local exhaust with heat forced off. `SERVICE` tests valve and fans with the heater hard-disabled.

## Workflow

1. Inspect and update requirements before code.
2. Capture U1 purifier telemetry and generated filament G-code read-only.
3. Freeze command semantics, units, timeout policy, arbitration, and safety behavior.
4. Validate the assembled controller with the heater disconnected.
5. Implement firmware only after explicit requirements approval.
6. Verify low-voltage behavior before mains testing.

Do not commit or push changes without Alex's explicit approval.
