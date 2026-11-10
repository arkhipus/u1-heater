# U1-driven design review

Status: requirements phase. This review intentionally contains no firmware reference implementation. The existing sketch is preserved as a historical prototype but is not the design authority for this branch.

## Decision

The controller will be redesigned around U1-driven intent:

```text
U1 UI / filament profile
        ↓
Moonraker purifier telemetry
        ↓
ESP32 arbitration and safety
        ↓
External heater box actuators
```

The hardware schematic, enclosure, purchased components, OLED, and five-position switch remain unchanged for now.

## Confirmed actuator constraints

- External recirculation blower: binary on/off, no PWM, no tachometer.
- PTC heater: binary on/off, thermostatically controlled.
- Outdoor exhaust fan: PWM-capable and intended to track a validated U1 exhaust percentage.
- Butterfly valve: servo-operated, but automatic operation uses fixed closed/open endpoints. U1 exhaust currently appears to be an on/off request rather than a flap-angle request.
- Heater protection: blower-powered hardware interlock, KSD301 manual-reset thermostat, and one-shot thermal fuse.

The U1's native purifier RPM is not evidence that the external blower is moving air.

## Confirmed U1 integration direction

The live U1 exposes purifier state including mode, desired chamber temperature, exhaust-fan request, inner-fan request, and fan-related status. The 3MF profile inspected for this project emits `SET_PURIFIER_MODE` commands for warm/recirculation and cooling behavior.

Before implementation, the following must be measured and documented:

- exact meaning and range of purifier modes;
- exact units and range of exhaust speed for mapping to external PWM; inner-fan speed is an external blower enable request;
- how the UI sliders alter telemetry;
- how filament recirculation settings alter generated G-code;
- automatic purge and cooldown timing;
- whether manual UI commands persist, expire, or are replaced by print-start commands.

## Five-position interface

The physical switch is defined as:

1. `OFF` - all external outputs safe/off;
2. `AUTO` - U1-driven operation;
3. `PREHEAT` - local thermostatic chamber preheat;
4. `PURGE` - local exhaust with heater forced off;
5. `SERVICE` - valve and fan tests with heater hard-disabled.

Priority is local `OFF`, `PURGE`, `SERVICE`, and `PREHEAT` over U1 `AUTO` requests. The final arbitration behavior must be tested before firmware is written.

## Safety requirements

The heater must be disabled on boot, reset, invalid switch state, invalid or stale telemetry, invalid cavity temperature, over-temperature, purge/exhaust, blower command off, loss of hardware heater permission, or internal fault.

Because there is no external-blower tachometer, the design must not claim to detect a stalled blower in software. The hardware relay interlock prevents heater power when the blower power path is absent. An airflow switch or current sensor is an optional future enhancement.

The PTC control is binary hysteresis, not PWM. A candidate 50 °C target with 49/50 °C switching thresholds is documented for review, not yet frozen.

## Requirements still requiring a decision

- Immediate heater shutdown versus a telemetry grace period.
- Final preheat target and timeout.
- Automatic purge bed-temperature gate and manual-purge behavior.
- Whether external exhaust follows U1 PWM continuously or uses bounded presets when U1 reports only an on/off exhaust request.
- Valve-open validation method.
- Minimum heater on/off dwell times.
- Whether a relay-status sense will be added to the ESP32.
- Exact startup and fault valve position.

## Verification sequence

1. Capture U1 telemetry for UI slider and filament-profile changes.
2. Freeze command mapping and arbitration requirements.
3. Validate switch decoding, valve movement, blower switching, and exhaust interlock with the heater disconnected.
4. Validate telemetry-loss, malformed-response, over-temperature, and reset behavior.
5. Only then implement firmware.
6. Perform mains testing last, after electrical inspection.

## Explicitly removed from this document

The former code excerpts, line-by-line firmware audit, and prototype implementation claims have been removed. Firmware will be reviewed after the requirements are approved.
