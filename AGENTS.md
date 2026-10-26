# AGENTS.md — Snapmaker U1 Chamber Controller

Context and rules for OpenClaw and Codex agents working in this repository.

---

## 1. Project Overview

This repository (`u1-heater`) contains the complete firmware, electrical schematics, 3D printable CAD models, and engineering documentation for an intelligent chamber temperature controller and dual-path exhaust system for the **Snapmaker U1 CoreXY 3D printer**.

* **Primary Objective:** Closed-loop active chamber heating (50 °C target) with internal carbon scrubbing for warp-free printing of high-temp engineering filaments (ASA, ABS, PC), plus automated outdoor fume exhaust and PLA/PETG cooling.
* **Brain:** ESP-WROOM-32 microcontroller communicating over Wi-Fi with the printer's Klipper / Moonraker REST API (polling every 2 seconds).

---

## 2. Repository Layout

* `arduino/` — ESP32 Arduino C++ firmware sketch (`.ino`), library dependencies, and configuration.
* `docs/esp32_chamber_controller_notes.md` — **Ground-truth engineering reference**. Contains full hardware pinouts, circuit schematics, BOM, Moonraker JSON schemas, and state machine architecture.
* `docs/ESP32_review.md` — Design review notes, safety decisions, and rationale.
* `docs/chamber_controller_schematic.drawio` — Electrical wiring diagram.
* `3dprint/` — CAD enclosure files (STEP, 3MF, STL) for the heater box, butterfly valve, servo mount, and duct adapters.
* `README.md` — High-level architecture and hardware overview.

---

## 3. Hardware Pinout & Wiring Specifications

All GPIO assignments refer to the 30-pin ESP-WROOM-32 NodeMCU board:

| Component | Pin | Logic / Type | Notes |
| :--- | :---: | :--- | :--- |
| **MG90S Servo** | `GPIO 18` | 5V PWM (via 5V buck) | 0° = Closed (Recirc), 90° = Open (Exhaust) |
| **Noctua NF-F12 Fan** | `GPIO 19` | 3.3V PWM (25 kHz) | 24V industrial PPC fan for outdoor exhaust |
| **I2C SDA (OLED)** | `GPIO 21` | 3.3V I2C Data | Dual color 128x64 SSD1306 OLED (0x3C) |
| **I2C SCL (OLED)** | `GPIO 22` | 3.3V I2C Clock | Powered strictly at 3.3V from ESP32 rail |
| **24V Blower MOSFET** | `GPIO 23` | 3.3V Digital OUT | NOYITO AOD4184 (ON/OFF only, no PWM) |
| **110V AC SSR Relay** | `GPIO 25` | **Active-LOW** Digital OUT | **LOW = ON (Heating), HIGH = OFF (Safe)** |
| **Mode Rotary Switch** | `GPIO 34` | Analog IN (ADC1) | 1-pin resistor ladder (OFF, LOW, MED, HI, PURGE) |

---

## 4. Architectural Rules & Safety Invariants

Any agent writing or modifying code in this repository must obey these rules:

1. **Active-LOW SSR Polarity:**
   * `PIN_HEATER_RELAY` is active-LOW. Driving `HIGH` cuts the heater. Driving `LOW` energizes the heater.
   * On boot, Wi-Fi timeout, or fault, `PIN_HEATER_RELAY` must default to `HIGH`.
2. **Never Run Exhaust Fan Against a Closed Valve:**
   * Inside `applyActuators()`, enforce the safety interlock: if `servoAngle == 0`, `noctuaPWM` must be forced to `0`. If `noctuaPWM > 0`, the valve must be forced to `90`.
3. **60-Second Watchdog Grace Period (Garage Wi-Fi Tolerance):**
   * If Wi-Fi disconnects or Moonraker telemetry fails to respond, do **NOT** instantly cut the heater. Maintain the last valid heating state for up to 60 seconds (`COMM_TIMEOUT_MS = 60000UL`) to survive brief connection drops.
   * If telemetry remains missing for $\ge 60$ seconds, immediately cut the heater (`PIN_HEATER_RELAY = HIGH`).
4. **Autonomous Purge Override (Option B):**
   * If Moonraker reports the printer is purging (`purifier.speed > 0` or `purifier.is_enabled`), the ESP32 must override all manual knob positions (**including `MODE_OFF`**), open the flap to 90°, and run the exhaust fan at 100%. The heater and internal recirculation blower must remain OFF during purge.
5. **Simultaneous Blower/Heater Start is Permitted:**
   * Blower startup delay is intentionally not required; simultaneous start with the PTC heater is approved.
6. **Persistent Hysteresis:**
   * In `MODE_HI` (target 50 °C), turn ON below 49.0 °C, turn OFF at/above 50.0 °C, and retain state in between. Include a hard software cutoff at 60.0 °C.

---

## 5. Development & Testing Workflow

* When testing the Moonraker integration without a live printer, run the mock Python server located in `docs/esp32_chamber_controller_notes.md`.
* Before modifying firmware, check `docs/esp32_chamber_controller_notes.md` Section 7 for the latest reference code.
