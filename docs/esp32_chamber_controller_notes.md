# ESP32 chamber controller engineering reference

Status: staged initial firmware implementation for the `u1-driven-design` branch. The heater-mode shutdown cooldown and Moonraker-unavailable valve policy amendments were approved and frozen on 2026-11-16.

This document is the engineering reference for the frozen staged baseline. Reuse display, network, and parsing helpers from the historical sketch where appropriate, but replace its obsolete manual-control behavior.

## 1. System boundary

The external controller contains:

- ESP32 controller;
- SSD1306 128x64 OLED;
- MG90S butterfly-valve servo;
- U1-PWM-driven outdoor exhaust fan, monitored by the ESP32;
- binary 24 V recirculation blower;
- binary 110 V PTC heater;
- blower-powered heater interlock relay;
- KSD301 manual-reset thermostat;
- one-shot thermal fuse;
- HEPA/carbon filtration path;
- 5 V buck converter and low-voltage wiring.

The Snapmaker U1 supplies chamber and print telemetry through Moonraker. The U1 touchscreen and filament profiles provide operating intent. The ESP32 applies safety rules and controls the external actuators.

The assembly uses the U1 onboard 24 V supply. U1 Pin 6 supplies the Noctua exhaust fan only; U1 Pin 5 directly drives its PWM input. GPIO 26 and GPIO 27 monitor the exhaust and recirculation requests, respectively.

The verified electrical schematic is the hardware authority; these notes do not change the hardware.

## 2. Air paths

### 2.1 Recirculation / heating path

```text
U1 chamber → recirculation inlet → carbon media → binary 24 V blower → PTC heater → U1 chamber
```

The butterfly valve is closed during recirculation and heating.

### 2.2 Exhaust path

```text
U1 chamber → butterfly valve open → outdoor exhaust fan → HEPA/carbon path → exterior duct
```

The outdoor exhaust fan must never run against a closed butterfly valve.

## 3. Actuator characteristics

| Actuator | Control | Feedback / protection |
|---|---|---|
| Recirculation blower | Binary ON/OFF | No tachometer. Blower-powered hardware heater interlock. |
| PTC heater | Binary ON/OFF | 3.3 V-compatible, active-high relay module on GPIO 25 (HIGH=ON, LOW=OFF), KSD301, one-shot thermal fuse, chamber control policy. Verify relay contact rating before mains operation. |
| Outdoor exhaust fan | PWM directly from U1 Pin 5 | ESP32 monitors the PWM request on GPIO 26; fan tach is not connected. |
| Butterfly valve | Servo, fixed closed/open endpoints in AUTO | U1 currently appears to request exhaust on/off, not a partial flap angle. Final open-state validation remains a requirement. |

A U1 purifier fan RPM field describes the U1's native fan. It does not verify the external recirculation blower.

## 4. U1 / Moonraker interface

The intended read-only query includes:

```text
print_stats
virtual_sdcard
temperature_sensor cavity
heater_bed
purifier
```

The controller is expected to use these purifier fields when present:

```text
purifier.power_detected
purifier.power_det_value
purifier.mode
purifier.desired_chamber_temp
purifier.critical_chamber_temp
purifier.inner_fan_rpm
purifier.inner_fan_work_time
purifier.exhaust_fan.speed
purifier.exhaust_fan.delay
purifier.exhaust_fan.speed_threshold
purifier.inner_fan.speed
purifier.inner_fan.delay
purifier.inner_fan.speed_threshold
```

#### ASA Print
```
      "purifier": {
        "power_detected": true,
        "power_det_value": 0.7,
        "mode": 3,
        "desired_chamber_temp": 45.0,
        "critical_chamber_temp": 0,
        "inner_fan_work_time": 7291.6,
        "exhaust_fan": {
          "speed": 0.0,
          "delay": 0,
          "speed_threshold": 0
        },
        "inner_fan": {
          "speed": 0.6,
          "delay": 600,
          "speed_threshold": 0.6
        }
      },
      "heater_bed": {
        "temperature": 100.0,
        "target": 100.0,
        "power": 0.402661787915018
      },
```

Checked-in captures and the synthetic mock payload are the staged parser examples. Required fields that are missing, malformed, unsupported, or outside the documented bounds must fail the heater OFF. Confirm exact live field presence, units, ranges, and semantics during bring-up with the heater disconnected.
During development, a non-actuating debug mode shall report raw/parsed Moonraker telemetry, GPIO 26/27 readings, commanded actuator states, Wi-Fi/API status, and heater inhibit reason over the serial debug console. It must not bypass or alter safety behavior.

### 4.1 U1 command interpretation

- `purifier.inner_fan.speed > 0` commands the external blower ON; `== 0` commands it OFF. This is binary control, not a speed command.
- Do not use `purifier.inner_fan_rpm` to command or verify the external blower; it reports the U1's internal fan.
- U1 Pin 5 directly drives the outdoor fan PWM input.
- GPIO 26 monitors the same exhaust PWM signal; the ESP32 does not generate or modify fan PWM.
- During valid telemetry, a positive validated Moonraker `purifier.exhaust_fan.speed` request or active GPIO 26 PWM commands the flap open. GPIO 26 must report a stable static level or 700-1300 Hz PWM across three consecutive samples before heater eligibility.
- Close the flap only after valid telemetry reports zero exhaust request and GPIO 26 reports stable inactive PWM for the release debounce. After the first valid telemetry response, if Moonraker data is unavailable or invalid, ignore the cached API exhaust request: active GPIO 26 PWM opens the flap, confirmed inactive PWM holds the current position, and unknown/stale PWM opens fail-safe. Do not open solely because the 60-second heater grace expires. GPIO 27 is diagnostic only.
- The external butterfly flap is binary in AUTO: closed only when both the API request and physical PWM are inactive, fixed open angle otherwise.
- No current filament or JSON field indicates a partial flap-angle request.
- `desired_chamber_temp > 0` is necessary but not sufficient for heating.
- The U1 Klipper source defines mode 0 as idle, 1 as cool chamber, 2 as preheat chamber, and 3 as hot chamber. Modes 2 and 3 are heater-eligible by this controller's policy; the numeric mapping alone does not prove which command the touchscreen emits.
- In Klipper, mode 0 requests both purifier fans off subject to shutdown delays (180 seconds by default); mode 1 runs the U1 exhaust fan for cooling; modes 2 and 3 run the U1 inner fan and turn the U1 exhaust fan off. These modes control U1 purifier fans, not the external PTC heater.
- `WAIT_CHAMBER_TEMP` is separate from `SET_PURIFIER_MODE MODE=2`; mode 2 alone does not enable the wait state. The mode-2 command defaults `DESIRE_TEMP` to 0, so the intended 50 °C initial target must be supplied by the UI/profile and confirmed in telemetry.
- Klipper reports `purifier.mode` as an integer in its status object; `GET_PURIFIER` separately formats a text mode name. Confirm the Moonraker query shape and actual UI/profile requests on the target machine.
- `purifier.power_detected` must be true before heater eligibility; `power_det_value` is diagnostic until its semantics are validated.
- `purifier.inner_fan_rpm` reports the U1 purifier's internal fan, not the external blower. `critical_chamber_temp` is a U1 cooling diagnostic and may be zero outside cooling mode.
- U1 internal-fan RPM and its fault/paused exception are diagnostic only; they do not gate the external heater.
- Heating also requires valid cavity temperature and a nonzero inner-fan request. Fan `delay` and `speed_threshold` are reported settings, not proof of fan operation.
- In modes 2 and 3, exhaust PWM below 20% may coexist with heating; at 20% or above, heat is OFF. GPIO 26 measures the U1 PWM request, not fan rotation.
- `print_stats.state` is used to identify print completion for the post-print vent/cooling inhibit; it does not, by itself, request heat. A candidate vent/cooling indication is mode 1 or an active exhaust request, pending validation on the target U1.
- The captured sample (`power_detected=false`, mode 0, target 0, both fan speeds 0, `print_stats.state="printing"`) is not heater-eligible. Print state alone does not override purifier availability or mode.
- An actual enabled request is captured in [petg_retry_20261108_220444.jsonl](../data/petg_retry_20261108_220444.jsonl): `power_detected=true`, mode 3, target 45 °C, exhaust speed 0, and inner-fan speed 0.6. However, the U1 `inner_fan_rpm` is 0; the next sample reports the printer paused with an internal-fan fault. Treat this as an enabled-but-faulted example, not a healthy operating baseline. The [mock API payload](../mock_api/api.py) shows a healthy-looking synthetic example, not a live capture.
- The ESP32 must not silently interpret missing numeric fields as zero.
- All parsed temperatures and speeds must be bounded and finite.

The [U1 Klipper purifier implementation](https://github.com/Snapmaker/u1-klipper/blob/7ac453b50fe6779b2ec0b759038a17da5e6d671d/klippy/extras/purifier.py) defines the numeric modes and command behavior. Its [virtual SD implementation](https://github.com/Snapmaker/u1-klipper/blob/7ac453b50fe6779b2ec0b759038a17da5e6d671d/klippy/extras/virtual_sdcard.py) records `SET_PURIFIER_MODE` lines found in print G-code. The checked [Fluidd Tune view](https://github.com/Snapmaker/u1-fluidd/blob/main/src/views/Tune.vue) is generic; the U1 touchscreen action and the target printer's emitted commands still need to be captured systematically before they become firmware assumptions.

## 5. Control requirements

### 5.1 U1-driven operation

The ESP32 shall:

- follow U1 purifier intent;
- turn the external blower on/off as a binary actuator;
- control the PTC with persistent temperature hysteresis;
- command the valve open for a positive validated Moonraker exhaust request or active GPIO 26 PWM;
- command the valve closed only after the API request and GPIO 26 are confirmed inactive through the release debounce;
- leave Noctua PWM generation to U1 Pin 5; the ESP32 only monitors that signal;
- force heat off during post-print venting/cooling regardless of mode or exhaust duty, in mode 1, and at or above 20% exhaust duty in modes 2/3;
- show U1 mode and telemetry state.

### 5.2 U1 preheat

When U1 reports a validated preheat/heating intent, the ESP32 shall:

- close the valve;
- start the external blower;
- regulate the PTC around the configured target;
- enforce a 20-minute maximum preheat duration;
- disable heat on invalid telemetry or reset. The KSD301 and one-shot thermal fuse provide independent over-temperature protection. The hardware interlock independently removes heater power; the ESP32 cannot detect relay permission.

The runtime setpoint comes from the validated U1 `purifier.desired_chamber_temp` field. Configure the initial U1 preheat target to 50 °C. The 20-minute timer applies only during preheat, not during target maintenance while printing. At timeout, latch heat OFF until U1 leaves mode 2 and later re-enters it; validate this re-arm behavior against the target U1. Use a 2 °C hysteresis band: heater ON at or below target minus 2 °C and OFF at or above target. Do not add minimum on/off dwell during initial debugging; log switching behavior and revisit if testing shows rapid cycling. Do not implement a separate software over-temperature cutoff; the KSD301 and one-shot thermal fuse remain mandatory.

### 5.3 Print-end venting/cooling

The post-print heater inhibit is derived from a finished `print_stats.state` plus purifier telemetry showing venting or cooling; there is no separate `purge` field or Klipper mode. The initial candidate uses terminal states `complete`, `cancelled`, or `error` together with mode 1 or a positive exhaust request; validate the state spellings and transition on the target U1. When active, this condition forces heat off regardless of mode or exhaust duty. Outside it, mode 1 is ineligible and modes 2/3 use the 20% exhaust threshold. The valve opens for a positive validated API request or active GPIO 26 PWM and closes only after both are confirmed inactive; the blower follows the inner-fan request.

### 5.4 Heater-mode shutdown cooldown

Start a 30-second external-blower cooldown when valid telemetry leaves mode 2 or 3 for an ineligible mode, or when the existing 60-second transport-loss grace expires, but only if the heater command was ON immediately before the shutdown. The heater turns OFF; the blower remains commanded ON for the timer even if the U1 inner-fan request becomes zero. No cooldown is started if the heater was already OFF or if it turns OFF through ordinary temperature regulation while the U1 remains in mode 2 or 3. Invalid safety-critical telemetry or an internal fault cancels the cooldown immediately. Reset or power loss ends the timer.

### 5.5 Frozen bed-target-triggered idle chamber preheat

The read-only manual heat-soak capture showed bed target 100 °C while `print_stats.state` was `standby`, no SD file was active, extruder targets were zero, and purifier mode/target/fan requests were all zero. The approved supplemental request requires fresh valid telemetry with bed target at least 95 °C, standby print state, no active SD file, `purifier.power_detected == true`, and purifier mode 0. Missing or invalid bed-target or SD-activity data disables only this supplemental request; normal U1 mode-2/3 policy remains available. The feature uses a 50 °C external chamber target and the existing 2 °C hysteresis, commands the external blower ON, and retains existing cavity-temperature, exhaust, GPIO 26, and other safety gates.

The supplemental request ends below the 95 °C bed-target threshold, when print state leaves standby, an SD file becomes active, or an existing fail-safe applies. On print start it relinquishes control; the ordinary U1 mode-2/3 policy may take over only with its normal valid chamber target and fan request. Do not carry the supplemental 50 °C target into printing. Apply the existing 20-minute timeout and latch heat OFF until the idle bed-trigger condition clears and is later re-entered. If the heater was ON when the request ends and normal U1 heater policy does not take over, use the existing 30-second blower cooldown. This amendment was approved and frozen on 2026-11-16 and is implemented; validate it with the heater disconnected.

## 6. Heater safety

The heater shall be OFF by default and on any of the following:

- boot or reset;
- before the first valid telemetry response;
- transport-level telemetry loss after the 60-second grace period;
- missing, non-finite, or implausible cavity temperature;
- post-print vent/cooling inhibit or exhaust request when the frozen mode-specific policy inhibits heat;
- blower command OFF;
- internal fault.

The existing hardware chain remains mandatory:

```text
24 V blower power → interlock relay permission → heater relay module contact → PTC
```

The KSD301 and one-shot thermal fuse are the required independent over-temperature protection; no separate software over-temperature cutoff is specified. Software must not claim that a commanded blower proves airflow. An airflow switch or current sensor is an optional future addition.

The blower-powered relay is an independent hardware cutoff in the heater power path. The current wiring has no relay-status input to the ESP32, so firmware must not claim to detect loss of relay permission.

After valid telemetry has been received, transport-level failure starts a configurable 60-second heater grace period. Continue retries and hold the last heater command without applying stale temperatures; if valid telemetry does not return by expiry, turn heat OFF and display `Telemetry lost`. Continue Wi-Fi/API reconnection attempts while heat is off. Invalid safety-critical data disables heat immediately. During Moonraker loss or invalid data, the flap follows the GPIO 26 fallback rule above; it does not open solely because the heater grace expires. The runtime target is U1 `desired_chamber_temp`, with the initial U1 preheat target set to 50 °C. Retry interval, poll interval, and request timeout remain to be frozen before code implementation.

## 7. Electrical pin assignments

These assignments are retained from the existing hardware design and are not being changed in this branch:

| Function | ESP32 GPIO | Notes |
|---|---:|---|
| Butterfly servo | 18 | 5 V servo supply; verify signal and current handling. |
| U1 recirculation PWM monitor | 27 | Input through 10 kΩ series resistor; verify input voltage. |
| U1 exhaust PWM monitor | 26 | Input through 13 kΩ series and 22 kΩ to ground. |
| Binary recirculation blower driver | 23 | 24 V MOSFET module; validate logic level. |
| Heater relay module | 25 | Active-high: HIGH=ON, LOW=OFF. Its VCC is 3.3 V; follow the verified schematic for the module's other control-side supply connections. Confirm contact rating before mains operation. |
| OLED SDA/SCL | 21 / 22 | 3.3 V I2C. |

## 8. OLED requirements

The existing 128x64 OLED layout is retained: a compact title band with print percentage/progress at the upper right, a horizontal separator, and a vertical separator between two content columns.

During bed-triggered idle chamber preheat, the title shows `U1 IDLE HEAT` instead of `U1 IDLE`.

```text
AUTO [U1]          42% ▓▓▓
────────────────────────────
CHBR: 50C     │ TRGT: 50C
HTR: ON       │ RCLN: ON
VLV: OPEN     │ EXST: 60%
TMRY: OK      │ CPU: 42C
```

The four content rows must fit the existing default-font column widths. The normal view shall show:

- effective mode and actuator requests; command source if reported by telemetry;
- actual and desired chamber temperature;
- heater ON/OFF;
- external blower command ON/OFF;
- valve command/state;
- U1-reported exhaust request percentage; physical GPIO 26 duty remains available in serial diagnostics;
- telemetry link state;
- filtered ESP32 internal temperature estimate.

Show a space after each label's colon and show chamber and target temperatures as whole-number Celsius values. Use the labels `CHBR`, `TRGT`, `HTR`, `RCLN`, `VLV`, `EXST`, and `TMRY`; use `VLV: CLOSE` for the closed state to fit the half-column.

The final row doubles as the message row. Faults, stale telemetry, Wi-Fi loss, or post-print vent/cooling status replace the normal `TMRY:OK | CPU:xxC` values. Examples include `TMRY:LOST` and `WIFI:OFF`. Print progress, bed/nozzle temperatures, and print state may appear in the title or a secondary page, but must not displace the normal thermal and actuator fields.

The `CPU:` value is a smoothed reading from the ESP32's on-chip sensor, not a calibrated CPU or chamber temperature. It is sampled with the display refresh and exponentially filtered (`alpha=0.1`, approximately 2.4 seconds time constant) for readability; display it for reference only, with no warning or heater-shutdown behavior. Show `--C` if the sensor read is invalid. The display shall not claim external blower RPM because no tachometer exists; `RCLN:ON` means commanded on, not proven airflow.

## 9. Staged validation sequence

The initial implementation baseline, heater-mode shutdown cooldown, and bed-target-triggered idle-preheat amendment are frozen. Use the existing captures and synthetic mock as examples, then observe live U1 behavior during bring-up. Start with the heater disconnected:

1. Capture Moonraker purifier telemetry at idle.
2. Change the U1 exhaust slider and record values.
3. Change the U1 recirculation slider and record values.
4. Test filament recirculation settings and generated G-code.
5. Test U1 mode 1 cooling, mode 2 preheat, and mode 3 hot-chamber behavior.
6. Capture print-finished transition into venting/cooling and the resulting purifier telemetry.
7. Resolve command units and arbitration.
8. With the heater disconnected, use `arduino/tests/servo_test/servo_test.ino` to verify 90° open / 180° closed motion and mechanical clearance, then validate GPIO 26/27 PWM decoding and flap response.
9. Validate blower relay and heater-permission wiring.
10. Test invalid JSON, missing fields, stale telemetry, reset, and simulated temperature changes across the 2 °C hysteresis band.
11. Verify with mock telemetry that a mode 2/3 to ineligible-mode transition keeps the blower ON for 30 seconds only when the heater was ON; verify invalid telemetry and internal faults cancel it.
12. With heater command ON, stop Moonraker telemetry; verify heat remains ON during the 60-second grace, then turns OFF while the blower stays ON for 30 more seconds.
13. Verify with telemetry loss/invalid data that active GPIO 26 PWM opens, confirmed inactive PWM holds position, and unknown/stale PWM opens fail-safe.
14. Test the 95 °C bed-target/standby/no-active-file trigger, mode-0 guard, missing/invalid optional-field handling, safety gating, 50 °C hysteresis, 20-minute timeout and re-arm, 30-second afterrun, and handoff to U1 mode 2/3 at print start.
15. Perform mains testing only after electrical inspection and explicit approval.

### Bench-only relay indicator test

For Arduino IDE, set `U1_BENCH_TEST` to `1` in `arduino/esp32_heater.cpp/bench_test_config.h`, then compile and upload `esp32_heater.cpp.ino`. Restore it to `0` and rebuild before returning to the production firmware. Arduino CLI can instead enable it with `arduino-cli compile --fqbn esp32:esp32:esp32 --build-property compiler.cpp.extra_flags=-DU1_BENCH_TEST=1 arduino/esp32_heater.cpp`.

Both builds use the polarity selected by `HEATER_RELAY_ACTIVE_LOW` in `bench_test_config.h`; the default is active-high (HIGH=ON, LOW=OFF) for the 3.3 V relay module. The bench flag bypasses only the GPIO 26 heater inhibit; all other heater-policy checks remain active, including valid telemetry, eligible mode, target/chamber bounds, blower request, exhaust request, preheat timeout, and the 60-second transport-loss grace. Use the bench build only with the PTC disconnected and an indicator substituted; a 110 V pilot light is still a mains load. This verifies control and relay switching, not PTC heating, airflow, or temperature-sensor accuracy.

The mock server accepts `--mode`, `--target`, `--chamber`, `--inner-fan`, `--exhaust`, `--bed-target`, `--print-state`, and `--sd-inactive` arguments. For example, `python3 mock_api/api.py --mode 3 --target 50 --chamber 44.2 --inner-fan 1 --exhaust 0` requests heat and blower operation. To simulate idle bed preheat, use `python3 mock_api/api.py --mode 0 --bed-target 100 --chamber 44.2 --print-state standby --sd-inactive`; this requests the fixed 50 °C chamber target and external blower. Stop the API to test the existing 60-second telemetry grace.

## 10. Out of scope

- Firmware implementation.
- Reference code snippets.
- Electrical schematic redesign.
- Component substitutions.
- Mains testing.
- Claims of external-blower stall detection without an added sensor.
