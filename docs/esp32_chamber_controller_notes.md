# ESP32 chamber controller engineering reference

Status: requirements phase for the `u1-driven-design` branch.

This document defines the intended system behavior before firmware implementation. It deliberately contains no reference firmware code. The existing Arduino sketch is a historical prototype and is not authoritative.

## 1. System boundary

The external controller contains:

- ESP32 controller;
- SSD1306 128x64 OLED;
- five-position resistor-ladder switch;
- MG90S butterfly-valve servo;
- PWM-capable outdoor exhaust fan;
- binary 24 V recirculation blower;
- binary 110 V PTC heater;
- blower-powered heater interlock relay;
- KSD301 manual-reset thermostat;
- one-shot thermal fuse;
- HEPA/carbon filtration path;
- 5 V buck converter and low-voltage wiring.

The Snapmaker U1 supplies chamber and print telemetry through Moonraker. The U1 UI and filament profiles provide operating intent. The ESP32 arbitrates intent and applies the safety rules before driving the external actuators.

The existing electrical schematic and purchased hardware are unchanged by this requirements branch.

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
| PTC heater | Binary ON/OFF | SSR/relay, KSD301, one-shot thermal fuse, chamber software limits. |
| Outdoor exhaust fan | PWM, tracking a validated U1 exhaust percentage | Valve interlock; fan command and fault state. |
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
purifier.mode
purifier.desired_chamber_temp
purifier.critical_chamber_temp
purifier.exhaust_fan.speed
purifier.inner_fan.speed
purifier.exhaust_fan.delay
purifier.inner_fan.delay
```

The exact field presence, units, ranges, and semantics must be captured from the target U1 before implementation.

### 4.1 U1 command interpretation

- A positive `inner_fan.speed` is an external-blower ON request, not a speed command.
- A zero `inner_fan.speed` is an external-blower OFF request.
- A positive exhaust request asks for the valve to open and the outdoor fan to run.
- The external exhaust PWM should follow the U1 exhaust percentage when the field is available and validated.
- The external butterfly flap is binary in AUTO: closed at no exhaust request, fixed open angle at any positive exhaust request.
- No current filament or JSON field indicates a partial flap-angle request.
- `desired_chamber_temp > 0` is necessary but not sufficient for heating.
- Heating also requires a U1 heating/preheat mode and recirculation requested.
- The ESP32 must not silently interpret missing numeric fields as zero.
- All parsed temperatures and speeds must be bounded and finite.

The 3MF inspected for this project emits U1 `SET_PURIFIER_MODE` commands for warm/recirculation and cooling modes. The generated commands and UI behavior must be captured systematically before they become firmware assumptions.

## 5. Five-position switch

The existing switch is retained with this requirements-level mapping:

| Position | Effective mode | Behavior |
|---|---|---|
| 1 | `OFF` | Heater, blower, and exhaust off; safe valve behavior. |
| 2 | `AUTO` | Follow U1 intent through ESP32 safety logic. |
| 3 | `PREHEAT` | Local closed-loop chamber preheat. |
| 4 | `PURGE` | Local valve-open exhaust with heater off. |
| 5 | `SERVICE` | Test valve and fans; heater hard-disabled. |

Priority is:

```text
OFF > PURGE > SERVICE > PREHEAT > AUTO
```

The switch is a local authority selector. It is not a fan-speed selector.

## 6. Control requirements

### 6.1 AUTO

In `AUTO`, the ESP32 shall:

- follow U1 purifier intent;
- turn the external blower on/off as a binary actuator;
- control the PTC with persistent temperature hysteresis;
- open the valve to the fixed open endpoint before enabling exhaust;
- map the validated U1 exhaust percentage to external fan PWM;
- prevent exhaust with a closed valve;
- force heat off during purge/cooling;
- preserve a visible indication of U1 versus local control source.

### 6.2 PREHEAT

In `PREHEAT`, the ESP32 shall:

- close the valve;
- start the external blower;
- regulate the PTC around the configured target;
- enforce a maximum preheat duration;
- disable heat on invalid telemetry, over-temperature, reset, or interlock loss.

A 50 °C target with 49/50 °C hysteresis is a candidate value, not yet a frozen specification.

### 6.3 PURGE

In local `PURGE`, the ESP32 shall:

- force the heater off;
- command the valve open;
- enable outdoor exhaust only after the chosen valve-open validation policy;
- keep the recirculation blower off unless needed for a safe transition.

Automatic U1 purge/cooldown and explicit local purge may have different bed-temperature policies. That distinction must be approved before implementation.

### 6.4 SERVICE

In `SERVICE`, the ESP32 shall:

- hard-disable the heater;
- exercise the valve;
- switch the binary recirculation blower;
- test outdoor exhaust only with the valve open;
- show commands, temperatures, interlock state, and faults;
- never display fabricated external-blower RPM.

## 7. Heater safety

The heater shall be OFF by default and on any of the following:

- boot or reset;
- invalid switch state;
- no validated telemetry;
- stale or failed Moonraker response;
- missing, non-finite, or implausible cavity temperature;
- software over-temperature;
- purge or exhaust request;
- blower command OFF;
- loss of the hardware blower-interlock permission;
- internal fault.

The existing hardware chain remains mandatory:

```text
24 V blower power → interlock relay permission → heater relay/SSR → PTC
```

The KSD301 and one-shot thermal fuse remain independent emergency protection. Software must not claim that a commanded blower proves airflow. An airflow switch or current sensor is an optional future addition.

The fail-safe response to telemetry loss, the final software cutoff, minimum relay dwell times, and target temperature must be explicitly frozen before code implementation.

## 8. Electrical pin assignments

These assignments are retained from the existing hardware design and are not being changed in this branch:

| Function | ESP32 GPIO | Notes |
|---|---:|---|
| Butterfly servo | 18 | 5 V servo supply; verify signal and current handling. |
| Outdoor exhaust PWM | 19 | Electrical interface validation required. |
| Binary recirculation blower driver | 23 | 24 V MOSFET module; validate logic level. |
| Heater relay/SSR | 25 | Active-low behavior must be verified before mains connection. |
| Switch ADC | 34 | Input-only ADC1 resistor ladder. |
| OLED SDA/SCL | 21 / 22 | 3.3 V I2C. |

The previous U1 purge-sense GPIO assignment is not assumed to be required by the new design until the signal and its electrical levels are verified.

## 9. OLED requirements

The existing 128x64 OLED layout is retained: a compact title band with print percentage/progress at the upper right, a horizontal separator, and a vertical separator between two content columns.

```text
AUTO [U1]          42% ▓▓▓
────────────────────────────
CH:50.2C      │ T:50.0C
H:ON          │ R:ON
V:OPEN        │ X:060%
TL:OK         │ CPU:42C
```

The four content rows must fit the existing default-font column widths. The normal view shall show:

- effective mode and command source;
- actual and desired chamber temperature;
- heater ON/OFF;
- external blower command ON/OFF;
- valve command/state;
- exhaust PWM percentage;
- telemetry link state;
- ESP32 internal CPU temperature.

The final row doubles as the message row. Faults, stale telemetry, Wi-Fi loss, purge/cooldown status, and high CPU temperature replace the normal `TL:OK | CPU:xxC` values. Examples include `TL:LOST`, `WIFI:OFF`, and `CPU:HIGH`. Print progress, bed/nozzle temperatures, and print state may appear in the title or a secondary page, but must not displace the normal thermal and actuator fields.

The CPU reading is the ESP32 die temperature, not chamber temperature. It is diagnostic and may trigger a conservative heater shutdown, but warning and shutdown thresholds must be validated on the assembled board during chamber testing. A heatsink does not replace the independent thermal cutoff. The display shall not claim external blower RPM because no tachometer exists; `R:ON` means commanded on, not proven airflow.

## 10. Validation sequence

Before writing firmware:

1. Capture Moonraker purifier telemetry at idle.
2. Change the U1 exhaust slider and record values.
3. Change the U1 recirculation slider and record values.
4. Test filament recirculation settings and generated G-code.
5. Test U1 mode 1 cooling and mode 3 warm/preheat behavior.
6. Capture automatic purge and cooldown behavior.
7. Resolve command units and arbitration.
8. Validate all switch positions with the heater disconnected.
9. Validate valve/exhaust interlock.
10. Validate blower relay and heater-permission wiring.
11. Test invalid JSON, missing fields, stale telemetry, reset, and over-temperature simulation.
12. Perform mains testing only after electrical inspection and explicit approval.

## 11. Out of scope

- Firmware implementation.
- Reference code snippets.
- Electrical schematic redesign.
- Component substitutions.
- Mains testing.
- Claims of external-blower stall detection without an added sensor.
