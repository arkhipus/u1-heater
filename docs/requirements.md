# U1-driven chamber controller requirements

Status: implementation baseline. This document supersedes the previous firmware-centric operating-mode design for the `u1-driven-design` branch.

## 1. Scope and design intent

The controller is an ESP32 companion to the Snapmaker U1. The U1 UI and filament profiles are the primary source of operating intent. The ESP32 is the safety and actuator layer for the external heater box.

The existing hardware, enclosure, front-panel OLED, valve, blower, heater, filters, and wiring schematic remain in scope. The five-position mode selector is removed from the control architecture and its input is unused or reserved.

The design must support:

- U1-driven automatic operation during printing.
- U1 UI control for manual exhaust, purge, and recirculation requests.
- U1-driven chamber preheat.
- Independent hardware protection for the 110 V heater.

There is no local mode selector in the redefined design. The ESP32 follows U1 intent and provides safety enforcement, actuator sequencing, and telemetry display.

## 1.1 Validated hardware wiring (2026-11-10)

The implementation must follow the assembled wiring, not the historical prototype pin map:

- U1 Pin 6 is a switched U1 output and is not the external 24 V power source.
- External 24 V comes from the UI power supply into the heater box and powers the Noctua exhaust fan, external recirculation blower, MOSFET module, relay coil, and buck converter.
- U1 Pin 2 is the common DC signal ground.
- U1 Pin 1 is monitored by GPIO 27 through the tested 10 kΩ series resistor.
- The Noctua exhaust fan receives continuous +24 V from the external UI supply; U1 Pin 5 alone drives its blue PWM input and is also monitored by GPIO 26 through 13 kΩ series and 22 kΩ to ground. The fan's 24 V power must not come from U1 Pin 6.
- U1 Pin 3 is PA6 inner-fan tachometer and remains N/C externally; no external tachometer is available.
- U1 Pin 4 is PA7 FLAG2 accessory-detection signal. Its use is reserved for validated accessory detection and it must not be confused with the inner-fan tachometer.
- GPIO 19 is unused and must not be connected to the Noctua PWM wire.
- GPIO 23 drives the low-side MOSFET; the blower and AEDICO relay coil share its switched return.
- GPIO 25 drives the heater SSR. The AEDICO relay contact and SSR contact are in series in the heater live path.

The permanent Pin 1 input protection remains a known hardware concern because the tested 10 kΩ-only connection is not a full voltage divider; measure GPIO 27 voltage before final enclosure installation.

## 2. Authoritative inputs

The controller shall poll Moonraker over the trusted local network. The intended U1 purifier telemetry includes:

- `purifier.mode` (U1 source defines idle `0`, cool-chamber `1`, preheat-chamber `2`, hot-chamber `3`; verify the live telemetry representation)
- `purifier.desired_chamber_temp`
- `purifier.critical_chamber_temp`
- `purifier.exhaust_fan.speed`
- `purifier.inner_fan.speed`
- purifier fan delays and enabled state when available
- `temperature_sensor cavity.temperature`
- `heater_bed.temperature`
- `print_stats.state`
- `virtual_sdcard.progress` when available

The exact response schema must be validated against the target U1 before firmware implementation. The standard bed-heating control must not be assumed to activate purifier mode; on the target U1, manual chamber preheat requires a validated `SET_PURIFIER_MODE` command, custom Klipper/Moonraker macro, or slicer start sequence.

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

## 3. U1-driven operating behavior

U1 is the sole source of operating intent. The ESP32 does not generate a competing local mode, fan speed, or purge command. The U1 purifier firmware defines `MODE_PREHEAT_CHAMBER = 2` and `MODE_HOT_CHAMBER = 3`, and exposes `SET_PURIFIER_MODE`; the exact touchscreen/web UI action and live command arguments still require validation on the target machine.

### 3.1 Exhaust/flap behavior

- U1 Pin 5 remains the sole Noctua PWM output.
- GPIO 26 monitors the Pin 5 PWM signal; GPIO 19 remains unused.
- Any validated nonzero U1 exhaust PWM request opens the external butterfly flap to its fixed open endpoint.
- A validated zero PWM request closes the flap to its fixed closed endpoint.
- The ESP32 must apply a short debounce/hold time to prevent servo chatter around zero.
- The flap is an enable gate, not a proportional airflow control. It does not track PWM duty.
- On telemetry loss, the ESP32 may preserve a safe cooldown/exhaust state, but must never enable the heater.

### 3.2 U1-following behavior

In normal operation:

- U1 mode and chamber target determine whether heating is requested.
- A nonzero U1 inner-fan request permits the external blower to run.
- A positive exhaust request opens the valve before enabling external exhaust.
- External exhaust PWM follows the validated U1 request percentage, clamped to the external fan's safe range.
- The external butterfly flap uses fixed closed/open endpoints; servo percentage is not treated as airflow percentage.
- The external blower remains binary.
- The PTC remains binary and thermostatic.
- U1 purge/cooldown commands override normal heating.

The ESP32 must not infer a heating request from fan speed alone. Heating requires a valid positive chamber target and a U1 heating/preheat intent.

### 3.3 Purge and preheat

Purge and preheat are selected through the U1 UI/profile controls. The ESP32 observes the resulting U1 requests rather than issuing a local override:

- Any positive U1 exhaust PWM opens the flap.
- U1 inner-fan/recirculation request turns the external blower ON as a binary actuator.
- U1 heating intent plus valid temperature telemetry allows thermostatic heater control.
- Heater is forced OFF immediately on invalid telemetry, over-temperature, blower command OFF, exhaust safety conflict, or hardware-interlock loss.
- Manual U1 purge therefore works without a physical selector: U1 commands exhaust, Pin 5 drives the Noctua, and GPIO 26 causes the ESP32 to open the flap.
- No direct ESP32 drive of GPIO 19 or the Noctua PWM wire is used.

## 4. Safety requirements

### 4.1 Heater fail-safe

The heater shall default OFF during:

- ESP32 reset or boot;
- invalid or unavailable U1 control state;
- no validated telemetry;
- stale or failed Moonraker response;
- invalid, missing, non-finite, or implausible cavity temperature;
- over-temperature;
- active U1 purge or exhaust when the safety policy requires heater inhibition;
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

### 4.3 Valve and exhaust sequencing

- A brief fan-start interval while the servo moves to the open endpoint is acceptable.
- On every observed exhaust state transition, apply the direct rule: `exhaust_pwm > 0 → flap_open`; `exhaust_pwm = 0 → flap_closed`.
- Any positive Pin 5/GPIO 26 exhaust activity shall command the servo open immediately; a transition back to zero shall command the servo closed after the configured short release debounce.
- Normal sustained exhaust operation requires the flap to reach its fixed open endpoint; the implementation shall use a bounded servo-movement delay for actuator settling, not as a different control decision.
- Partial servo opening is not part of automatic operation unless a future measurement proves it useful.
- On controller boot, reset, or fault, the heater remains OFF. If exhaust PWM is active, the valve shall move/open rather than remain intentionally closed against the running fan.
- When exhaust PWM returns to zero, the servo may close after a short release delay.

### 4.4 Mains separation

110 V AC wiring remains physically and electrically separate from low-voltage logic. No firmware requirement may weaken the existing mains isolation, fuse, thermal cutoff, connector, strain-relief, grounding, or enclosure requirements.

## 5. Telemetry and command handling

- Poll interval, timeout, retry policy, and stale-data limit must be specified before implementation.
- JSON fields must be explicitly checked; missing fields must not silently become zero.
- Numeric values must be finite, bounded, and unit-documented.
- U1 exhaust-speed units must be confirmed for display and GPIO 26 validation; the ESP32 does not generate or modify this PWM.
- The U1 exhaust request is an enable/request signal for the valve: any positive Pin 5/GPIO 26 activity opens the external flap to its fixed open position.
- No U1 or filament setting currently establishes a partial butterfly-flap angle. Automatic operation shall use closed/open endpoints only.
- The controller must distinguish U1 native purifier fan RPM from external actuator state.
- Every command source and arbitration decision should be visible on the OLED.

## 6. OLED requirements

The existing 128x64 OLED layout is retained. It uses a compact title band, a horizontal separator, and a vertical separator between two content columns:

```text
U1 FOLLOW          42% ▓▓▓
────────────────────────────
CH:50.2C      │ T:50.0C
H:ON          │ R:ON
V:OPEN        │ X:060%
TL:OK         │ CPU:42C
```

The title's upper-right area retains print percentage and the existing graphical progress indicator when a print is active. The top-left area shows the U1-reported effective mode and command source. The four content rows use the existing vertical split; each half-column must remain short enough for the default font.

Required normal-view fields:

- U1 mode and command source;
- actual chamber temperature;
- desired chamber temperature;
- heater state;
- external recirculation blower command state;
- exhaust valve state;
- observed U1 exhaust PWM activity/duty, when measurable;
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

The physical selector is not part of this validation; verify that its input is electrically isolated or left unused.

Then validate the assembled controller with the heater disconnected:

- boot and reset states;
- selector input disconnected/unused;
- valve/exhaust interlock;
- blower relay behavior;
- simulated telemetry loss and malformed responses;
- simulated over-temperature;
- U1 command arbitration.

Mains heater testing is a later phase and requires explicit electrical verification first.

## 8. Out of scope for this branch

- Changes to the electrical schematic.
- Changes to the purchased components.
- Mains testing.
- Final pinout changes.
- Claims that software alone can detect an external blower stall.
