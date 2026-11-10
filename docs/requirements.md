# U1-driven chamber controller requirements

Status: requirements phase. This document supersedes the previous firmware-centric operating-mode design for the `u1-driven-design` branch. No firmware implementation is implied by these requirements.

## 1. Scope and design intent

The controller is an ESP32 companion to the Snapmaker U1. The U1 UI and filament profiles are the primary source of operating intent. The ESP32 is the safety and actuator layer for the external heater box.

The existing hardware, enclosure, front-panel OLED, five-position switch, valve, blower, heater, filters, and wiring schematic remain in scope and are not redesigned by this branch.

The design must support:

- U1-driven automatic operation during printing.
- U1 UI control for manual exhaust and recirculation requests.
- Local chamber preheat.
- Local emergency purge.
- A heater-safe service/test mode.
- Independent hardware protection for the 110 V heater.

## 2. Authoritative inputs

The controller shall poll Moonraker over the trusted local network. The intended U1 purifier telemetry includes:

- `purifier.mode`
- `purifier.desired_chamber_temp`
- `purifier.critical_chamber_temp`
- `purifier.exhaust_fan.speed`
- `purifier.inner_fan.speed`
- purifier fan delays and enabled state when available
- `temperature_sensor cavity.temperature`
- `heater_bed.temperature`
- `print_stats.state`
- `virtual_sdcard.progress` when available

The exact response schema must be validated against the target U1 before firmware implementation.

### 2.1 Binary recirculation interpretation

The external recirculation blower is on/off only. It has no PWM and no tachometer.

Therefore:

- A nonzero U1 inner/recirculation request means **blower requested ON**.
- A zero request means **blower requested OFF**.
- The configured percentage is an intent or profile parameter, not an external blower speed.
- The ESP32 shall not attempt to PWM the external recirculation blower.

The U1's native purifier RPM, if reported, describes the U1's internal fan and must not be treated as proof that the external blower is operating.

### 2.2 Binary heater interpretation

The PTC heater is on/off only. The ESP32 shall control it with thermostatic hysteresis through the existing heater relay/SSR path.

A proposed default target is 50 °C, subject to final validation. A proposed control band is:

- heater ON below 49 °C;
- heater OFF at or above 50 °C;
- heater forced OFF at the independent software over-temperature limit;
- hardware KSD301 and thermal fuse remain the final protection layer.

The final target, hysteresis, minimum on/off times, and software cutoff must be approved before code work begins.

## 3. Operating modes

The five-position switch shall be redesigned as:

| Position | Name | Intent |
|---|---|---|
| 1 | `OFF` | Force heater, blower, and exhaust off; valve closed after safe motion policy. |
| 2 | `AUTO` | Follow U1 purifier intent and filament-profile commands through ESP32 safety logic. |
| 3 | `PREHEAT` | Locally request closed-loop chamber preheat, independent of a print profile. |
| 4 | `PURGE` | Locally force valve open and exhaust; heater off. |
| 5 | `SERVICE` | Exercise valve and fans for assembly/testing; heater permanently inhibited. |

The switch is an authority selector, not a second speed controller.

### 3.1 Priority

The effective priority shall be:

```text
OFF > PURGE > SERVICE > PREHEAT > AUTO
```

`AUTO` is the only mode in which U1 UI and filament-profile requests control the external system.

### 3.2 AUTO behavior

In `AUTO`:

- U1 mode and chamber target determine whether heating is requested.
- A nonzero U1 inner-fan request permits the external blower to run.
- A positive exhaust request opens the valve before enabling external exhaust.
- External exhaust PWM follows the validated U1 request percentage, clamped to the external fan's safe range.
- The external butterfly flap uses fixed closed/open endpoints; servo percentage is not treated as airflow percentage.
- The external blower remains binary.
- The PTC remains binary and thermostatic.
- U1 purge/cooldown commands override normal heating.

The ESP32 must not infer a heating request from fan speed alone. Heating requires a valid positive chamber target and a U1 heating/preheat intent.

### 3.3 PREHEAT behavior

In `PREHEAT`:

- Valve closed.
- External recirculation blower ON.
- PTC controlled by chamber temperature hysteresis.
- Fixed target selected by the requirements configuration, initially proposed as 50 °C.
- Maximum preheat duration required.
- Heater disabled on telemetry loss, invalid temperature, over-temperature, or loss of the hardware blower interlock.
- Transition to a safe non-heating state after timeout.

### 3.4 PURGE behavior

In local `PURGE`:

- Heater forced OFF.
- Recirculation blower OFF unless explicitly needed for a safe transition.
- Valve commanded open and given time to move.
- External exhaust enabled only after the valve-open condition is satisfied by the chosen validation method.
- No bed-temperature gate is required for an explicit local purge unless the final safety review adds one.

For automatic U1 purge/cooldown, retain the existing policy under review: wait for bed temperature at or below 60 °C before aggressive exhaust if that protects the print. The final distinction between automatic and manual purge must be documented before implementation.

### 3.5 SERVICE behavior

In `SERVICE`:

- Heater hard-disabled in firmware logic.
- Valve open/close test.
- External blower on/off test.
- External exhaust fan test with valve open only.
- OLED displays switch position, commands, temperature, and fault state.
- No fan RPM claim may be displayed for the external blower because it has no tachometer.
- A deliberate hold-to-enter or timeout is recommended to prevent accidental service activation.

## 4. Safety requirements

### 4.1 Heater fail-safe

The heater shall default OFF during:

- ESP32 reset or boot;
- invalid switch state;
- no validated telemetry;
- stale or failed Moonraker response;
- invalid, missing, non-finite, or implausible cavity temperature;
- over-temperature;
- active purge or exhaust;
- blower command OFF;
- loss of the hardware blower-interlock permission;
- any internal software fault.

The safe response to telemetry loss shall be decided explicitly before implementation. The baseline proposal is immediate heater OFF, with optional blower/exhaust cooldown behavior that cannot re-enable heat.

### 4.2 Blower interlock

The external blower has no tachometer. Heater permission shall therefore rely on:

1. ESP32 blower command ON;
2. the existing 24 V hardware interlock relay energized by the blower power path;
3. relay contacts in series with heater permission;
4. independent KSD301 and thermal-fuse protection.

An airflow switch or current sensor may be added later, but is not required for this requirements baseline. A commanded blower is not equivalent to proven airflow.

### 4.3 Valve and exhaust interlock

- Exhaust fan must never run against a closed valve.
- Any positive exhaust command must first produce the fixed open-valve state.
- Partial servo opening is not part of automatic operation unless a future measurement proves it useful.
- Servo movement and startup position must be defined.
- On controller fault, the valve and exhaust behavior must be chosen explicitly; heater OFF is mandatory.

### 4.4 Mains separation

110 V AC wiring remains physically and electrically separate from low-voltage logic. No firmware requirement may weaken the existing mains isolation, fuse, thermal cutoff, connector, strain-relief, grounding, or enclosure requirements.

## 5. Telemetry and command handling

- Poll interval, timeout, retry policy, and stale-data limit must be specified before implementation.
- JSON fields must be explicitly checked; missing fields must not silently become zero.
- Numeric values must be finite, bounded, and unit-documented.
- U1 exhaust-speed units must be confirmed, then mapped to the external exhaust PWM as a bounded percentage.
- The U1 exhaust request is an enable/request signal for the valve: any positive exhaust request opens the external flap to its fixed open position.
- No U1 or filament setting currently establishes a partial butterfly-flap angle. Automatic operation shall use closed/open endpoints only.
- The controller must distinguish U1 native purifier fan RPM from external actuator state.
- Every command source and arbitration decision should be visible on the OLED.

## 6. OLED requirements

The existing 128x64 OLED layout is retained. It uses a compact title band, a horizontal separator, and a vertical separator between two content columns:

```text
AUTO [U1]          42% ▓▓▓
────────────────────────────
CH:50.2C      │ T:50.0C
H:ON          │ R:ON
V:OPEN        │ X:060%
TL:OK         │ CPU:42C
```

The title's upper-right area retains print percentage and the existing graphical progress indicator when a print is active. The top-left area shows the effective mode and command source. The four content rows use the existing vertical split; each half-column must remain short enough for the default font.

Required normal-view fields:

- effective mode and command source;
- actual chamber temperature;
- desired chamber temperature;
- heater state;
- external recirculation blower command state;
- exhaust valve state;
- external exhaust PWM percentage;
- telemetry link state (`TL:OK` or equivalent);
- ESP32 internal temperature (`CPU:xxC`).

The final row is also the message row. A fault, stale telemetry, Wi-Fi loss, purge/cooldown message, or high CPU-temperature warning takes priority over the normal `TL:OK | CPU:xxC` display. Examples include `TL:LOST`, `WIFI:OFF`, and `CPU:HIGH`. Print state, bed temperature, and nozzle temperature may appear in the title or a secondary status page, but must not crowd the normal thermal/actuator view.

The CPU value is the ESP32 die temperature, not chamber temperature. It is diagnostic and may provide a conservative heater-shutdown trigger, but its warning and shutdown thresholds must be validated on the assembled board during chamber testing. A heatsink does not replace the independent thermal cutoff. The display must not show a fabricated external-blower RPM; `R:ON` means commanded on, not proven airflow.

## 7. Verification plan before firmware

The following must be captured from the U1 without changing hardware:

1. Telemetry with purifier idle.
2. UI exhaust slider at zero and nonzero values.
3. UI recirculation slider at zero and nonzero values.
4. Filament profiles with recirculation disabled and enabled.
5. Filament recirculation percentages at representative values.
6. Mode 1 cooling and mode 3 heating/preheat commands.
7. Automatic print-end purge and delay behavior.
8. Manual purge behavior.
9. Exact units and ranges for every fan-speed field.

Then validate the assembled controller with the heater disconnected:

- boot and reset states;
- all five switch positions;
- valve/exhaust interlock;
- blower relay behavior;
- simulated telemetry loss and malformed responses;
- simulated over-temperature;
- U1 command arbitration.

Mains heater testing is a later phase and requires explicit electrical verification first.

## 8. Out of scope for this branch

- Firmware implementation.
- Changes to the electrical schematic.
- Changes to the purchased components.
- Mains testing.
- Final pinout changes.
- Claims that software alone can detect an external blower stall.
