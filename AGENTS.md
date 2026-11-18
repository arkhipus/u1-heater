# AGENTS.md - U1-driven design branch

This repository defines an ESP32 companion controller for the Snapmaker U1 chamber heater, recirculation path, and outdoor exhaust.

## Current phase

The staged requirements baseline in `docs/requirements.md` was approved for initial firmware implementation on 2026-11-14. Implement against that baseline, reusing useful display, network, and parsing helpers from the historical sketch while removing its obsolete manual-control behavior. Do not modify the verified electrical schematic or perform mains testing in this phase.

## Authoritative documents

- `docs/requirements.md` - authoritative requirements and verification plan.
- `docs/esp32_chamber_controller_notes.md` - engineering reference.
- `README.md` - project overview.
- `docs/chamber_controller_schematic.drawio` and `docs/chamber_controller_schematic-Copy of Page-2.pdf` - verified hardware references.

## Design constraints

- U1 UI and filament profiles provide operating intent through Moonraker purifier telemetry.
- ESP32 provides arbitration, safety, and external actuator control.
- The assembly uses the U1 onboard 24 V supply. U1 Pin 6 supplies the Noctua exhaust fan only.
- External recirculation blower is binary on/off and has no tachometer.
- PTC heater is binary on/off and thermostatically controlled.
- The runtime heater target comes from U1 `purifier.desired_chamber_temp`; initial U1 preheat target is 50 °C.
- U1 Pin 5 directly drives Noctua PWM; GPIO 26 monitors exhaust PWM and GPIO 27 monitors recirculation PWM.
- GPIO 19 does not drive the Noctua PWM input.
- The flap follows the observed exhaust request: command it open while exhaust PWM is active and closed when inactive.
- Heater defaults OFF on boot and reset; missing or invalid safety-critical data, blower command OFF, or internal fault also disable heat. The KSD301 and one-shot fuse provide independent over-temperature protection; firmware does not add a separate over-temperature cutoff. After valid telemetry, a transport failure holds the last heater command for a 60-second grace, then turns heat OFF.
- Existing blower-powered heater interlock, KSD301 thermostat, and one-shot thermal fuse remain mandatory.
- Do not claim external-blower airflow or RPM verification without adding a sensor.
- Keep 110 V AC separate from low-voltage wiring.
- Never connect unverified U1 signals directly to ESP32 GPIO.

## Workflow

1. Follow the frozen staged baseline; fail the heater OFF on missing, invalid, or unsupported safety-critical telemetry.
2. Use checked-in captures and the synthetic mock only as initial parser examples; validate live U1 behavior during bring-up.
3. Validate low-voltage control and actuator behavior with the heater disconnected.
4. Verify low-voltage behavior before connecting the heater or considering mains testing.

Do not commit or push changes without Alex's explicit approval.
