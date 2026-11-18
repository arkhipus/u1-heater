# U1-driven chamber controller requirements

Status: baseline, post-heater blower-cooldown, Moonraker-unavailable valve, and bed-target-triggered idle chamber-preheat amendments are frozen for staged initial firmware implementation (baseline approved 2026-11-14; other amendments approved 2026-11-16). This document supersedes the previous firmware-centric operating-mode design for the `u1-driven-design` branch. Values identified as bring-up assumptions must be validated before heater operation.

## 1. Scope and design intent

The controller is an ESP32 companion to the Snapmaker U1. The U1 UI and filament profiles are the primary source of operating intent. The ESP32 is the safety and actuator layer for the external heater box.

The existing hardware, enclosure, front-panel OLED, valve, blower, heater, filters, and wiring schematic remain in scope. The U1 touchscreen and filament profiles are the operating interface.

The design must support:

- U1-driven automatic operation during printing.
- U1 UI control for exhaust and recirculation requests, plus automatic post-print venting/cooling.
- U1-driven chamber preheat.
- Independent hardware protection for the 110 V heater.

The ESP32 follows U1 intent and provides safety enforcement, actuator control, and telemetry display.

## 1.1 Validated hardware wiring (2026-11-10)

The implementation must follow the assembled wiring, not the historical prototype pin map:

- The assembly is powered by the U1 onboard 24 V supply. U1 Pin 6 supplies the Noctua exhaust fan only.
- U1 Pin 2 is the common DC signal ground.
- U1 Pin 1 is monitored by GPIO 27 through the tested 10 kΩ series resistor.
- U1 Pin 5 directly drives the Noctua blue PWM input and is also monitored by GPIO 26 through R3 (13 kΩ series) and 22 kΩ to ground. The separate 15 kΩ component shown in the verified PDF is not R3 and remains unchanged.
- U1 Pin 3 is PA6 inner-fan tachometer and remains N/C externally; no external tachometer is available.
- U1 Pin 4 is PA7 FLAG2 and is connected to Pin 2 through the 1 kΩ presence resistor shown in the verified schematic. It is not the inner-fan tachometer.
- GPIO 19 is unused and must not be connected to the Noctua PWM wire.
- GPIO 23 drives the low-side MOSFET; the blower and AEDICO relay coil share its switched return.
- GPIO 25 drives the 3.3 V-compatible, active-high heater relay module (HIGH = ON, LOW = OFF). The AEDICO interlock contact and heater relay contact are in series in the heater live path. Verify the replacement relay's AC contact rating against heater current before mains operation.

The Pin 1 input uses a 10 kΩ series resistor rather than a voltage divider. Verify the GPIO 27 voltage against ESP32 input limits before final enclosure installation.

## 2. Authoritative inputs

The controller shall poll Moonraker over the trusted local network. The intended U1 purifier telemetry includes:

- `purifier.power_detected` (must be true before heater eligibility)
- `purifier.power_det_value` (diagnostic; do not use as a control input without validated semantics)
- `purifier.mode` (Klipper defines idle `0`, cool-chamber `1`, preheat-chamber `2`, and hot-chamber `3`; verify the live Moonraker object path and representation)
- `purifier.desired_chamber_temp`
- `purifier.critical_chamber_temp` (U1 cooling diagnostic; do not treat zero as a valid external-heater limit)
- `purifier.inner_fan_rpm` when available (U1 internal-fan diagnostic, not external-blower feedback)
- `purifier.inner_fan_work_time` (diagnostic)
- `purifier.exhaust_fan.speed`, `.delay`, and `.speed_threshold`
- `purifier.inner_fan.speed`, `.delay`, and `.speed_threshold`
- `temperature_sensor cavity.temperature`
- `heater_bed.temperature`, `.target`, and `.power`
- `print_stats.state`
- `virtual_sdcard.is_active` for the frozen idle-preheat guard, plus
  `virtual_sdcard.progress` when available

The [U1 Klipper purifier source](https://github.com/Snapmaker/u1-klipper/blob/7ac453b50fe6779b2ec0b759038a17da5e6d671d/klippy/extras/purifier.py) defines these numeric modes and returns `mode` as an integer in the Klipper `purifier` status object. Its `GET_PURIFIER` G-code response also adds a text mode name; do not assume that Moonraker's object query returns that text form. Initial firmware shall use the checked-in captures as observed examples and fail the heater OFF if required fields are missing, malformed, unsupported, or outside the documented bounds. Verify the live response schema and touchscreen/profile actions during staged bring-up, with the heater disconnected. If `power_detected` is false, heater eligibility is false. Use fan `speed` values as the current requests; `delay` and `speed_threshold` are additional reported settings, not proof of fan operation. `inner_fan_rpm` refers to the U1 purifier fan, not the external blower. `print_stats.state` helps identify print completion but is not, by itself, a heater request. The standard bed-heating control must not be assumed to activate purifier mode; the separate frozen idle-preheat amendment below requires explicit additional guards.

### 2.1 Binary recirculation interpretation

The external recirculation blower is on/off only. It has no PWM and no tachometer.

Therefore:

- If `purifier.inner_fan.speed > 0`, command the external blower ON.
- If `purifier.inner_fan.speed == 0`, command the external blower OFF.
- The configured percentage is an intent or profile parameter, not an external blower speed.
- The ESP32 shall not attempt to PWM the external recirculation blower.
- Do not use `purifier.inner_fan_rpm` to command or verify the external blower; it reports the U1's internal fan.

The U1's native purifier RPM, if reported, describes the U1's internal fan and must not be treated as proof that the external blower is operating.
U1 internal-fan RPM and fault/paused status are diagnostic only and do not gate the external heater; heater eligibility depends on the external blower command and independent hardware interlock, without claiming verified external airflow.

#### Heater-mode shutdown cooldown

When valid telemetry changes from heater-eligible mode 2 or 3 to an ineligible mode, start a 30-second external-blower cooldown only if the heater command was ON immediately before the transition. Also start the cooldown when the existing 60-second transport-loss grace expires while the heater command is ON. In either case, turn the heater OFF and keep the blower commanded ON for 30 seconds even if the U1 inner-fan request is zero. Do not start a cooldown when the heater was already OFF, or for an ordinary thermostat cutoff while the U1 remains in a heater-eligible mode. Invalid safety-critical telemetry and internal faults cancel an active cooldown immediately. Reset or loss of controller power also ends the cooldown.

### 2.2 Binary heater interpretation

The PTC heater is on/off only. The ESP32 shall control it with thermostatic hysteresis through the 3.3 V-compatible, active-high heater relay module on GPIO 25 (HIGH = ON, LOW = OFF).

The runtime target shall come from `purifier.desired_chamber_temp` and remain configurable through the U1 UI/API. The intended initial U1 preheat target is 50 °C. Klipper's `SET_PURIFIER_MODE MODE=2` handler defaults `DESIRE_TEMP` to `0`, so 50 °C is not guaranteed by the mode command itself; verify the UI/profile supplies it during bring-up. Do not replace a missing or invalid API target with a silent firmware default. Heater eligibility requires `power_detected == true`, a mode and target that permit heating, valid cavity temperature, and a nonzero inner-fan request. The post-print vent/cooling condition described in Section 3 inhibits heating.

The initial 50 °C preheat value is a configuration choice, not a hard-coded override of a valid U1 target. Use a 2 °C hysteresis band: turn the heater ON at or below target −2 °C and OFF at or above target. Do not add minimum on/off dwell during initial debugging; log switching behavior and revisit only if testing shows rapid cycling. Do not implement a separate software over-temperature cutoff; the KSD301 and one-shot thermal fuse remain mandatory independent protection.

Preheat is limited to 20 minutes. This limit applies only during preheat; it does not limit heater operation while maintaining chamber temperature during an active print. At timeout, heat is disabled. For purifier mode-2 preheat, re-arm behavior is intentionally deferred until U1 behavior during preheat is observed on the printer; do not silently restart heating at timeout. Bed-triggered preheat uses its separate latch and re-arm rule below.

#### Frozen amendment: bed-target-triggered idle chamber preheat

The live read-only capture showed the manual heat-soak condition with `heater_bed.target = 100 °C`, `print_stats.state = "standby"`, `virtual_sdcard.is_active = false`, extruder targets at zero, and purifier mode/target/fan requests all zero. Based on that observation, the approved supplemental preheat rule is:

- Start external chamber preheat only when fresh, valid Moonraker telemetry reports `heater_bed.target >= 95 °C`, `print_stats.state == "standby"`, `virtual_sdcard.is_active == false`, and `purifier.power_detected == true`, while purifier mode is idle (`0`). The bed target, standby state, and SD activity fields are supplemental inputs; if any are missing or invalid, this rule does not activate. Existing mode-2/3 requests retain precedence and are never overridden by the fixed bed-preheat target.
- Use a 50 °C external chamber target and the existing 2 °C heater hysteresis. Command the external recirculation blower ON while this supplemental preheat is active; bed temperature and bed power are context/diagnostic fields, not external-heater setpoints.
- Retain all independent heater safety gates: valid cavity temperature, no post-print venting, no mode-1 cooling, no validated or physical exhaust request at/above the existing 20% inhibit, and a known/stable GPIO 26 signal. Do not operate heat with an unknown exhaust state.
- End bed-triggered preheat if bed target falls below 95 °C, the print state leaves `standby`, an SD file becomes active, or any existing heater fail-safe applies. When the print starts, relinquish this special request; valid U1 mode 2/3 policy may take over using its own positive chamber target and fan request. Do not carry the bed-triggered 50 °C target into printing.
- Apply the existing 20-minute preheat timeout to this mode; latch heat OFF until the idle bed-trigger condition clears and is later re-entered. If the heater was ON when the bed-triggered request ends and normal U1 heater policy does not take over, use the existing 30-second blower cooldown.

This amendment was approved and frozen on 2026-11-16. It is implemented in the controller policy and must be validated with the heater disconnected before any mains testing.

## 3. U1-driven operating behavior

U1 is the source of operating intent. The ESP32 does not generate a competing local mode, fan speed, or purge command. The U1 Klipper [`purifier.py`](https://github.com/Snapmaker/u1-klipper/blob/7ac453b50fe6779b2ec0b759038a17da5e6d671d/klippy/extras/purifier.py) source confirms the numeric values and `SET_PURIFIER_MODE MODE=n` command:

- Mode 0 (idle) requests both purifier fans off, subject to their configured shutdown delays (180 seconds by default in Klipper).
- Mode 1 (cool chamber) runs the U1 exhaust fan and its cooling control; the U1 inner fan is off.
- Modes 2 (preheat chamber) and 3 (hot chamber) run the U1 inner fan and turn the U1 exhaust fan off.
- `WAIT_CHAMBER_TEMP` is a separate G-code command; selecting mode 2 alone does not enable its wait state.

These are U1 purifier fan behaviors, not commands to the project's external PTC heater. The external controller's policy is to make modes 2 and 3 heater-eligible and modes 0 and 1 ineligible; eligibility still requires a valid positive target, valid cavity temperature, and a nonzero recirculation request. Treat the numeric mapping as source-confirmed; validate that the target touchscreen/profile emits the expected mode, target, and fan requests during staged bring-up, with the heater disconnected. The checked [Fluidd Tune view](https://github.com/Snapmaker/u1-fluidd/blob/main/src/views/Tune.vue) is generic and the [U1 Moonraker repository](https://github.com/Snapmaker/u1-moonraker) does not define these mode semantics.

In modes 2 and 3, an observed exhaust PWM duty below 20% does not by itself inhibit heating. At 20% or above, the heater shall turn OFF; heating may resume when duty falls below 20% and all other heater conditions are satisfied. GPIO 26 measures the U1 PWM request, not fan rotation. The exact duty-measurement tolerance around 20% remains to be validated.

### 3.1 Exhaust/flap behavior

- U1 Pin 5 remains the sole Noctua PWM output.
- GPIO 26 monitors the Pin 5 PWM signal; GPIO 19 remains unused.
- U1 Pin 5 directly drives the Noctua PWM input; the ESP32 does not generate or modify this PWM.
- GPIO 26 observes Pin 5. An active exhaust PWM request commands the flap to its fixed open endpoint.
- An inactive exhaust PWM request commands the flap to its fixed closed endpoint after the release debounce.
- The ESP32 must apply a short debounce/hold time to prevent servo chatter around zero.
- The flap is an enable gate, not a proportional airflow control. It does not track PWM duty.
- The fan can start directly from the U1 request while the servo is moving; the ESP32 cannot sequence or delay the fan.
- After the first valid telemetry response, when Moonraker data is unavailable or invalid, ignore cached API exhaust values and use GPIO 26: active PWM opens the flap, confirmed inactive PWM holds the current flap position, and unknown/stale PWM opens fail-safe. GPIO 26 reports the U1 PWM request, not physical fan rotation.

### 3.2 U1-following behavior

In normal operation:

- U1 mode and chamber target determine whether heating is requested.
- A nonzero U1 inner-fan request permits the external blower to run.
- U1 Pin 5 directly drives the Noctua PWM input; GPIO 26 monitors the same request signal.
- The flap is commanded open while the exhaust PWM request is active and closed when it is inactive.
- The external butterfly flap uses fixed closed/open endpoints; servo percentage is not treated as airflow percentage.
- The external blower remains binary.
- The PTC remains binary and thermostatic.
- Heater inhibition follows the verified mode and exhaust-duty rules below, with a post-print vent/cooling condition as an unconditional heater-off override. Derive it from print-state and purifier telemetry, not from a separate purge label.

The ESP32 must not infer a heating request from fan speed alone. Heating requires a valid positive chamber target and a U1 heating/preheat intent.

### 3.3 Print-end venting/cooling and preheat

The post-print vent/cooling inhibit is active after a validated finished print state when purifier telemetry indicates venting or cooling. It is a derived condition, not a separate Moonraker `purge` field or Klipper mode. A candidate indication is mode 1 (cool chamber) or an active exhaust request, but the exact print-state values and telemetry condition must be confirmed on the target U1. `print_stats.state` alone does not determine heater eligibility.

U1 UI/profile controls generate purifier mode and fan requests. The ESP32 observes telemetry rather than issuing a local override:

- Any detected active U1 exhaust PWM request commands the flap open.
- U1 inner-fan/recirculation request turns the external blower ON as a binary actuator.
- U1 heating intent plus valid temperature telemetry allows thermostatic heater control.
- The post-print vent/cooling condition forces heat OFF regardless of purifier mode or exhaust duty.
- Outside that condition, mode 1 is heater-ineligible; in modes 2 and 3, exhaust duty below 20% may coexist with heating and duty at or above 20% forces heat OFF.
- Heater is forced OFF immediately on invalid telemetry, blower command OFF, or the defined exhaust threshold. The KSD301 and one-shot thermal fuse provide independent over-temperature protection. The hardware interlock independently removes heater power; the ESP32 has no signal to detect interlock permission.
- U1 commands exhaust, Pin 5 drives the Noctua, and GPIO 26 causes the ESP32 to open the flap.
- No direct ESP32 drive of GPIO 19 or the Noctua PWM wire is used.

## 4. Safety requirements

### 4.1 Heater fail-safe

The heater shall default OFF during:

- ESP32 reset or boot;
- invalid or unavailable U1 control state;
- before the first valid telemetry response;
- transport-level telemetry loss after the configured 60-second grace period;
- invalid, missing, non-finite, or implausible cavity temperature;
- mode-specific exhaust inhibition as defined in Section 3;
- the post-print vent/cooling condition;
- blower command OFF;
- any internal software fault.

The blower-powered interlock is hardware-only: it independently removes heater power when its power path is de-energized, but the ESP32 cannot detect relay permission. Firmware must not make behavior depend on sensing interlock state. Do not implement a separate software over-temperature cutoff; the KSD301 thermostat and one-shot thermal fuse are the required independent over-temperature protection.

The heater-mode shutdown cooldown does not override heater interlocks or other heater-off rules. It may keep the blower commanded ON for 30 seconds after the qualifying mode transition or transport-grace expiry in Section 2.1. Invalid safety-critical telemetry or an internal fault cancels it immediately.

After valid telemetry has been received, a transport-level failure starts a 60-second grace period. Continue retrying and hold the last heater command during this period; do not update the thermostat using stale temperatures. If valid telemetry returns within 60 seconds, resume control. Otherwise turn the heater OFF and display `Telemetry lost` on the OLED. Continue Wi-Fi/API reconnection attempts while heat is off. The 60-second grace is a configurable constant. Missing, malformed, non-finite, or implausible safety-critical fields disable the heater immediately. Fresh valid data is required before heat can resume after shutdown. The valve behavior during this grace period is specified below; behavior of other non-heater actuators remains TBD.

For the exhaust flap, after the first valid telemetry response, when Moonraker data is unavailable or invalid, ignore any cached API exhaust request and use GPIO 26: active PWM opens the flap, confirmed inactive PWM holds the current position, and unknown/stale PWM opens fail-safe. Do not open the flap solely because the 60-second heater grace expires. This does not change the heater's 60-second grace or post-expiry blower cooldown. GPIO 26 is an exhaust-request signal; without a fan sensor, the ESP32 cannot confirm physical fan rotation.

### 4.2 Blower interlock

The external blower has no tachometer. Heater permission shall therefore rely on:

1. ESP32 blower command ON;
2. the existing 24 V hardware interlock relay energized by the blower power path;
3. relay contacts in series with heater permission;
4. independent KSD301 and thermal-fuse protection.

The hardware relay is an independent cutoff; the current wiring has no ESP32-readable relay-status signal. Firmware must not claim to detect loss of relay permission or prove airflow. An airflow or relay-status sensor would be a separate hardware change.

### 4.3 Valve and exhaust sequencing

- U1 drives the Noctua directly, so the ESP32 cannot wait for the flap to reach its open endpoint before fan startup.
- A positive validated Moonraker `purifier.exhaust_fan.speed` request or active GPIO 26 exhaust PWM commands the flap open during valid telemetry.
- Command the flap closed only after valid telemetry reports zero exhaust request and GPIO 26 reports stable inactive PWM for the configured release debounce.
- When Moonraker data is unavailable or invalid after a valid response, ignore the cached API request: active GPIO 26 PWM opens the flap, confirmed inactive PWM holds the current position, and unknown/stale PWM opens fail-safe.
- GPIO 26 observes the U1 exhaust request. It does not prove that the fan is physically rotating because the fan tachometer is not connected.
- Partial servo opening is not part of automatic operation unless a future measurement proves it useful.
- On controller boot, reset, before the first valid telemetry response, or unknown/transitioning GPIO 26 input, the heater remains OFF as applicable and the valve shall be commanded open. After valid telemetry has been received, use the GPIO 26 fallback rule during Moonraker loss or invalid data; do not open solely because the 60-second heater grace expires.
- When both the validated API request and GPIO 26 indicate no exhaust, the servo may close after a short release delay.

### 4.4 Mains separation

110 V AC wiring remains physically and electrically separate from low-voltage logic. No firmware requirement may weaken the existing mains isolation, fuse, thermal cutoff, connector, strain-relief, grounding, or enclosure requirements.

## 5. Telemetry and command handling

- Initial implementation defaults: poll Moonraker every 2 seconds, use a 1500 ms HTTP timeout, and retry Wi-Fi reconnection every 10 seconds. The 60-second transport grace is defined in Section 4.1.
- JSON fields must be explicitly checked; missing fields must not silently become zero.
- Numeric values must be finite, bounded, and unit-documented. For the initial parser, chamber temperature is accepted from -20 to 150 °C and target from 0 to 100 °C; these are plausibility bounds, not a software over-temperature cutoff. The target must be positive for heat eligibility. Fan speed accepts either a 0-1 fraction or a 0-100 percentage; values from 0 through 1 are interpreted as fractions and values above 1 as percentages.
- Connection failures and HTTP 5xx responses use the 60-second transport grace; other non-200 responses, malformed JSON, and invalid or unsupported safety-critical fields disable heat immediately.
- U1 exhaust-speed units and reported speed versus GPIO 26 PWM agreement must be checked during bring-up; the ESP32 does not generate or modify this PWM. The conservative heater inhibit applies if either validated telemetry duty or confirmed physical PWM duty is at least 20%.
- GPIO 26 must report a stable static level or 700-1300 Hz PWM across three consecutive samples before heater eligibility. An unknown or transitioning signal keeps heat OFF and commands the flap open; GPIO 27 readings are diagnostic only.
- The U1 exhaust request is an enable/request signal for the valve: any positive validated Moonraker request or active Pin 5/GPIO 26 PWM opens the external flap to its fixed open position.
- No U1 or filament setting currently establishes a partial butterfly-flap angle. Automatic operation shall use closed/open endpoints only.
- Servo endpoints are 180° closed and 90° open; open immediately on boot/unknown PWM state and close only after 500 ms of confirmed inactive exhaust PWM. Verify mechanical clearance and endpoints with the heater disconnected.
- Initial post-print venting rule: `print_stats.state` equal to `complete`, `cancelled`, or `error`, together with a positive exhaust request or mode 1, inhibits heat. Validate terminal-state spellings and transition behavior on the target U1.
- Accept only `print_stats.state` values `standby`, `printing`, `paused`, `complete`, `cancelled`, and `error`; an unknown value is invalid telemetry and disables heat.
- A 20-minute preheat timeout latches heat OFF until U1 leaves mode 2 and later re-enters mode 2. This is the conservative initial re-arm behavior; revise only after observing U1 preheat behavior.
- No extra minimum heater on/off dwell is applied in the initial implementation; log cycling behavior and revisit if testing shows rapid switching.
- The controller must distinguish U1 native purifier fan RPM from external actuator state.
- Show effective state and any conflicts or overrides on the OLED. Display command source only when telemetry reports it; do not infer whether a request came from the touchscreen or a print profile.
- A non-actuating debug mode shall report raw/parsed Moonraker telemetry, GPIO 26/27 readings, commanded actuator states, Wi-Fi/API status, and heater inhibit reason. Debug output must not bypass or alter safety behavior.

## 6. OLED requirements

The existing 128x64 OLED layout is retained. It uses a compact title band, a horizontal separator, and a vertical separator between two content columns:

```text
U1 FOLLOW          42% ▓▓▓
────────────────────────────
CHBR: 50C     │ TRGT: 50C
HTR: ON       │ RCLN: ON
VLV: OPEN     │ EXST: 60%
TMRY: OK      │ CPU: 42C
```

At startup, show `Connecting to WiFi...` until the ESP32 obtains a network connection. Then show its full IPv4 address with a 10-second countdown; begin the countdown only after Wi-Fi connects. After the countdown, switch to the normal OLED view and do not replay the address screen on later Wi-Fi reconnects. In normal view, the title's upper-right area retains print percentage and the existing graphical progress indicator when a print is active. The top-left area shows the U1-reported effective mode and may show the command source if telemetry provides it. During bed-triggered idle chamber preheat, show `U1 IDLE HEAT` in place of `U1 IDLE`. The four content rows use the existing vertical split; each half-column must remain short enough for the default font.

Required normal-view fields:

- U1 mode and effective actuator requests; command source if available;
- actual chamber temperature;
- desired chamber temperature;
- heater state;
- external recirculation blower command state;
- exhaust valve state;
- U1-reported exhaust request percentage; physical GPIO 26 duty remains available in serial diagnostics;
- telemetry link state (`TMRY:OK` or equivalent);
- filtered ESP32 internal temperature estimate (`CPU:xxC`).

The final row is also the message row. A fault, stale telemetry, Wi-Fi loss, or post-print vent/cooling status takes priority over the normal `TMRY:OK | CPU:xxC` display. After the 60-second transport grace expires, display the error `Telemetry lost`. Other examples include `TMRY:LOST` and `WIFI:OFF`. Print state, bed temperature, and nozzle temperature may appear in the title or a secondary status page, but must not crowd the normal thermal/actuator view.

Show a space after each label's colon. Show chamber and target temperatures as whole-number Celsius values. Use the compact labels `CHBR`, `TRGT`, `HTR`, `RCLN`, `VLV`, `EXST`, and `TMRY`. Keep the longest strings within their 63-pixel columns at the default 6-pixel character width; use `VLV: CLOSE` for the closed state so it fits.

The `CPU:` value is a smoothed reading from the ESP32's on-chip sensor, not a calibrated CPU or chamber temperature. It is sampled with the display refresh and exponentially filtered (`alpha=0.1`, approximately 2.4 seconds time constant) for readability; display it for reference only, with no warning or heater-shutdown behavior. Show `--C` if the sensor read is invalid. The display must not show a fabricated external-blower RPM; `RCLN:ON` means commanded on, not proven airflow.

### Read-only HTTP status

The ESP32 shall expose `GET /api/status` on HTTP port 80 as a read-only JSON status endpoint. The response includes Wi-Fi/IP information; telemetry link state, freshness, age, print and purifier status; chamber/target/bed temperatures and fan requests; commanded heater/blower/valve state; the heater inhibit reason; measured exhaust PWM status; and the CPU-temperature estimate when available. Cached telemetry must be distinguished from fresh telemetry, and actuator fields describe commands rather than verified physical operation. Do not expose raw Moonraker payloads, credentials, or state-changing routes. Keep access within the trusted local network and do not port-forward the endpoint.

## 7. Staged verification plan

The following U1 behaviors remain to be observed during bring-up:

1. Telemetry with purifier idle.
2. UI exhaust slider at zero and nonzero values.
3. UI recirculation slider at zero and nonzero values.
4. Filament profiles with recirculation disabled and enabled.
5. Filament recirculation percentages at representative values.
6. Mode 1 cooling, mode 2 preheat, and mode 3 hot-chamber commands.
7. Print-finished transition into venting/cooling and delay behavior.
8. Manual exhaust and recirculation requests when no print is active.
9. Exact units and ranges for every fan-speed field.

Validate the assembled controller first with the heater disconnected:

- boot and reset states;
- servo 90° open / 180° closed endpoint motion using the standalone test with the heater disconnected;
- GPIO 26/27 signal decoding and flap response;
- blower relay behavior;
- with valid mode-2/3 telemetry and a heater command ON, change to an ineligible mode with the inner-fan request at zero; verify heater OFF and blower ON for 30 seconds;
- repeat the mode change with the heater already OFF and verify no extra cooldown run;
- verify invalid safety-critical telemetry or an internal fault cancels the cooldown immediately;
- with the heater command ON, stop Moonraker telemetry; verify heat remains ON during the existing 60-second grace, then turns OFF while the blower stays ON for 30 more seconds;
- after valid telemetry, verify Moonraker loss/invalid data uses GPIO 26 only: active PWM opens, confirmed inactive PWM holds the current valve position, and unknown/stale PWM opens fail-safe;
- with the heater disconnected, verify the 95 °C bed-target / standby / no-active-file trigger, mode-0 guard, missing/invalid optional-field handling, safety gating, 50 °C hysteresis, 20-minute timeout and re-arm, 30-second afterrun, and handoff to U1 mode 2/3 at print start;
- simulated telemetry loss and malformed responses;
- simulated temperature changes across the 2 °C hysteresis band;
- U1 command arbitration.

Only after low-voltage validation, install/connect the heater and verify control behavior with appropriate electrical safeguards. Mains testing remains a separate step requiring electrical inspection and explicit approval.

## 8. Out of scope for this branch

- Changes to the electrical schematic.
- Changes to the purchased components.
- Mains testing.
- Final pinout changes.
- Claims that software alone can detect an external blower stall.

## 9. Deferred printer validation

The following are explicit staged assumptions to validate and, if needed, revise after the initial low-voltage firmware bring-up. They do not authorize bypassing fail-safe behavior:

- Actual touchscreen/profile actions and the exact live Moonraker object schema.
- Post-print terminal-state spellings, vent/cooling transitions, and whether an additional post-exhaust heater hold is needed.
- Fan-speed units and agreement between telemetry and GPIO 26/27 PWM measurements.
- Servo endpoints, release delay, and maximum travel time.
- Non-heater actuator behavior during transport grace and after its expiry.
- Whether any minimum on/off dwell is needed after reviewing debug logs.
