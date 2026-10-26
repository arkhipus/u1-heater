# ESP32 Chamber Controller Review

Review of `esp32_chamber_controller_notes.md`.

The architecture is reasonable as a prototype: the ESP32 orchestrates the system, while independent thermal cutoffs provide a separate mains safety layer. The heater should not be energized with this firmware yet. The main problem is that the firmware is fail-open around telemetry and does not currently meet the safety claims made in the design.

## Critical issues

### 1. The firmware does not compile as pasted

Lines 361-365 contain string literals split across physical lines:

```cpp
Serial.println("
WiFi Connected! IP: " + WiFi.localIP().toString());
```

They should be written as:

```cpp
Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());
Serial.println("\nWiFi pending, moving to main loop...");
```

### 2. The documented relay failsafe is backwards

Line 241 says the heater is cut by driving the relay `LOW`, but the code defines the relay as active-low:

```cpp
digitalWrite(PIN_HEATER_RELAY, HIGH); // OFF
digitalWrite(PIN_HEATER_RELAY, LOW);  // ON
```

The documentation must say `HIGH` for heater-off. This contradiction is dangerous during troubleshooting.

### 3. Wi-Fi loss leaves the heater on for up to one minute

At lines 375-416, Wi-Fi loss enters `handleWifiDisconnected()`, but the heater is only disabled after `COMM_TIMEOUT_MS`, which is 60 seconds. The design section promises 10 seconds.

If telemetry was recently valid, the heater continues running during the entire grace period. For a heater, loss of valid telemetry should immediately disable heat. Fans may remain on if that is the intended safe state.

### 4. There is no explicit valid-telemetry state

`lastValidPollTime` starts at zero. The controller has no explicit state saying that a valid response has ever been received. Normal control should not run until a complete, validated response has arrived.

Use a flag such as:

```cpp
bool telemetryValid = false;
```

Set it only after validating every required field. Do not execute heater logic unless telemetry is valid and fresh.

### 5. Malformed or incomplete JSON is treated as valid

Lines 498-510 mark any successfully parsed JSON as valid. Missing fields silently become zero:

```cpp
chamberTemp = status["temperature_sensor cavity"]["temperature"] | 0.0;
```

A response missing the cavity sensor can therefore look like a fresh valid packet. Required objects and fields should be checked explicitly, and values should be validated as finite and within plausible ranges. Validation failure must disable the heater.

## Control-logic issues

### 6. Automatic purge does not override every knob position

The design says purge overrides any knob position, but this condition blocks automatic purge while the knob is OFF:

```cpp
if ((u1IsPurging || currentMode == MODE_PURGE) && currentMode != MODE_OFF)
```

Automatic purge should be handled first and independently of the rotary mode. The code also ignores `purifier.is_enabled`, even though the design says to use it.

### 7. Purifier speed units are undefined

This line assumes `purifier.speed` is a fractional value from 0 to 1:

```cpp
map(u1PurifierSpeed * 100, 0, 100, 0, 255)
```

If the API reports 0-100 instead, the result is invalid and may overflow. Define the unit, clamp the value, and convert it through a bounded helper.

### 8. The claimed hysteresis is not implemented

This resets the heater decision on every loop:

```cpp
bool shouldHeat = false;
if (chamberTemp < 49.0) {
  shouldHeat = true;
}
```

That is a threshold, not persistent hysteresis. Retain heater state and use separate on/off thresholds, for example 48.5 C and 50 C. Add an independent software over-temperature cutoff below the hardware thermostat limit.

<!-- ### 9. The blower and heater start simultaneously, 

HI mode enables both in one call:

```cpp
applyActuators(0, 0, true, shouldHeat);
```

Start the blower first, wait for confirmed airflow, then enable the heater. A tachometer or airflow switch is preferable to assuming that a commanded blower is moving air. The 121 C fuse is an emergency cutoff, not an airflow sensor. 

AA: This is not needed. Simultanious start is fine.

-->

### 10. Dead and contradictory state remains in the source

These are unused:

```cpp
PURGE_DURATION_MS
autoPurgeActive
autoPurgeStartTime
lastDisplayTime
DISPLAY_INTERVAL_MS
toolheadZ
activeTool
```

Remove them or implement the behavior they imply. In particular, clarify whether purge is API-controlled, timer-controlled, or both.

## Hardware and electrical concerns

- The Noctua PWM input may require an open-drain interface, pull-up, or level translation. Driving it directly from a 3.3 V GPIO may be unreliable.
- Confirm the exact NOYITO MOSFET module input wiring. An optocoupler module is not automatically 3.3 V compatible.
- Give the MG90S supply local bulk capacitance and verify that the buck converter handles servo current spikes.
- Add appropriate fusing on both the 24 V branches and the heater branch.
- Do not treat a generic 5-pin connector as an acceptable mains interface without a rated, keyed, touch-safe connector and proper strain relief. The proposed connector exposes live and safety-loop wiring to dangerous misconnection.
- The 15 mm air-gap recommendation is not a complete insulation specification. Creepage, material group, pollution degree, terminal ratings, enclosure construction, and applicable electrical code also matter.
- Consider a normally-closed safety relay or contactor arrangement where loss of control power guarantees heater shutdown.
- Verify relay or SSR power-up behavior and ensure the input cannot float during ESP32 reset.

## Smaller firmware concerns

- Configure ADC attenuation explicitly and add averaging, switch debouncing, and threshold hysteresis.
- Clamp print progress before converting it to a percentage or drawing the progress bar.
- Avoid showing the Wi-Fi SSID on the OLED unless that is intentional.
- Plain HTTP is only appropriate on a trusted isolated LAN. Do not commit real Wi-Fi credentials to source.
- Limit the HTTP response size before loading it into memory.
- Verify that the target U1 actually exposes the `purifier` Moonraker object and response shape assumed by the document.

## Recommended implementation order

1. Fix the compile errors and relay-polarity documentation.
2. Add strict telemetry validation and a `telemetryValid` state.
3. Force the heater off immediately on Wi-Fi loss, HTTP failure, invalid JSON, missing sensors, out-of-range temperatures, reset, or invalid switch state.
4. Make automatic purge a true override, including when the knob is OFF.
5. Implement persistent hysteresis, blower startup delay, airflow verification, and a software over-temperature cutoff.
6. Verify every module electrically before connecting mains.
7. Test boot, Wi-Fi loss, bad JSON, missing sensors, switch faults, ESP32 reset, blower disconnection, and relay-input failure with the heater disconnected.

## Overall assessment

The overall system concept is workable, but the current firmware has a compile blocker and several safety paths that can leave the heater energized without fresh, validated temperature data. Treat the hardware cutoffs as final emergency protection, not as compensation for the firmware behavior. Correct the fail-safe control path before proceeding to mains testing.
