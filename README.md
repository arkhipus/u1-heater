# Snapmaker U1 Chamber Heater & Exhaust Controller (`u1-heater`)

An intelligent, safety-interlocked chamber temperature controller and exhaust system for the Snapmaker U1 3D printer.

---

## Objective

The Snapmaker U1 is an enclosed CoreXY printer capable of printing high-performance engineering plastics, but materials like ASA, ABS, and PC require a warm, stable chamber (~50 °C) to prevent warping and layer delamination. Conversely, PLA and PETG require active ventilation to prevent heat creep, and printing styrene-based filaments creates toxic ultrafine particles and VOCs that must be cleared safely.

This project solves all three problems with a single compact external unit:
1. **Active Chamber Heating:** Maintains a regulated 50 °C chamber environment for warp-free ASA/PC printing.
2. **Internal Carbon Scrubbing:** Continuously recirculates air through an activated carbon pellet bed while heating.
3. **Automated Outdoor Exhaust:** Automatically vents toxic fumes directly outside through a 4" duct upon print completion, or provides controlled fresh-air draft for PLA/PETG.

---

## System Architecture & Approach

Rather than running an isolated, dumb thermostat that requires drilling holes for extra thermocouples, the controller acts as an intelligent companion node using the printer's existing sensors and network:

```
                  ┌──────────────────────────────────────────┐
                  │    Snapmaker U1 (Klipper / Moonraker)    │
                  │   Internal Cavity Temp / Print State     │
                  └─────────────────────┬────────────────────┘
                                        │ Wi-Fi REST API (2s poll)
                                        ▼
                  ┌──────────────────────────────────────────┐
                  │      ESP32 Microcontroller Core         │
                  │  - Mode State Machine                    │
                  │  - Watchdog & Failsafe Logic             │
                  │  - SSD1306 Flight Panel Dashboard        │
                  └───────┬──────────────┬─────────────┬─────┘
                          │              │             │
        ┌─────────────────┘              │             └──────────────────┐
        ▼                                ▼                                ▼
  [Recirculation Box]           [Exhaust Ducting]                 [Front Panel]
  - 100W PTC Air Heater         - Butterfly Flap (MG90S Servo)    - 5-Way Rotary Switch
  - 24V Radial Blower           - Noctua NF-F12 PPC 24V Fan       - 0.96" I2C OLED Display
  - Carbon Pellet Scrubber      - 4" Exterior Wall Coupler
```

### 1. Aerodynamic Switching (The Butterfly Valve)
The unit connects to the printer's side port and uses a center-pivot butterfly valve driven by an MG90S metal-gear servo to switch between two airflow paths:
* **Closed (0°):** Air stays entirely inside the printer. A 24V blower pulls chamber air through a Sukh activated carbon pellet tray and pushes it across a 100W PTC heater element.
* **Open (90°):** The recirculation circuit closes and a 3,000 RPM Noctua industrial PWM fan exhausts chamber air outdoors through a 4" flexible duct.

### 2. Multi-Layer Hardware & Firmware Safety
Because the heater operates on 110V AC mains inside a plastic housing, safety is layered so no single component failure can cause a hazard:
* **Hardware Blower Interlock:** A 24V mechanical relay coil is wired in parallel with the 24V blower terminals. If the blower loses power or its MOSFET fails, the relay opens and mechanically disconnects the 110V heater power line.
* **Thermal Cutoffs:** A 70 °C manual-reset KSD301 snap-action thermostat and a 121 °C one-shot thermal fuse (SF119E) sit directly on the PTC heater frame in series with the AC Live line.
* **Solid State Relay (SSR):** An Omron G3MB-202P active-LOW SSR controls AC power with zero-crossing switching.
* **Watchdog Failsafe:** If Wi-Fi drops or Moonraker stops responding, the firmware maintains its last valid heating state for a 60-second grace period (preventing print-ruining thermal shock during brief garage Wi-Fi glitches). If telemetry is lost for more than 60 seconds, the heater is cut immediately.
* **Software Interlocks:** The Noctua exhaust fan is locked out whenever the butterfly valve is at 0°, preventing blade stall and backpressure.

---

## Operating Profiles

Selected via the 5-position rotary knob on the front panel:

| Mode | Valve Flap | Noctua Exhaust | Internal Blower | 100W PTC Heater | Primary Target |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **OFF** | 0° (Closed) | 0% | OFF | OFF | Standby / Cold |
| **LOW** | 90° (Open) | ~70% (180 PWM) | OFF | OFF | PLA (Max chamber cooling) |
| **MED** | 90° (Open) | ~15% (40 PWM) | OFF | OFF | PETG (Gentle draft) |
| **HIGH** | 0° (Closed) | 0% | ON | 50 °C Closed-Loop | ASA / ABS / PC (Recirculating heat) |
| **PURGE**| 90° (Open) | 100% (255 PWM)| OFF | OFF | Manual evacuation |

### Autonomous Purge Override
When a print finishes, the Snapmaker U1 activates its internal purifier fan. The ESP32 detects this via Moonraker telemetry (`purifier.speed > 0` or `purifier.is_enabled`), overrides the current manual knob position (including `OFF`), opens the valve to 90°, and exhausts 100% outdoors while displaying an alert banner on the OLED.

---

## OLED Live Flight Panel

The 128x64 SSD1306 display provides real-time status:

```text
┌──────────────────────────────┐
│ HIGH [ASA]       74% [████░] │  <-- Yellow Band: Active Mode & Graphical Progress
│ ──────────────────────────── │
│ CHMB: 50.2°C  | HTR: ON      │  <-- Chamber cavity temp & PTC relay status
│ BED:  100°C   | NOZ: 260°C   │  <-- Live bed and nozzle temperatures
│ VLV:  CLOSED  | EXH:   0%    │  <-- Butterfly valve position & exhaust fan PWM %
│ TME: 01:42    | ETA: 00:28   │  <-- Elapsed print time & remaining ETA
└──────────────────────────────┘
```

---

## Repository Structure

```
u1-heater/
├── README.md               # Project overview and architecture reference
├── LICENSE                 # Project license
├── 3dprint/                # 3D printable CAD models (STL, STEP, 3MF)
│   └── ...                 # Heater enclosure, servo mount, butterfly flap, duct couplers
├── arduino/                # Firmware source code for ESP32
│   └── esp32_heater/       # Arduino sketch, secrets template, and libraries
└── docs/                   # Detailed technical documentation
    ├── esp32_chamber_controller_notes.md  # Complete engineering reference & pinouts
    ├── chamber_controller_schematic.drawio # Electrical and wiring schematic diagram
    └── ESP32_review.md     # Design review notes and audit history
```

---

## Hardware Bill of Materials (BOM)

* **Microcontroller:** ESP-WROOM-32 (30-pin NodeMCU form factor)
* **Display:** 0.96" I2C SSD1306 128x64 OLED (Yellow/Blue dual color)
* **Mode Selector:** 5-position rotary switch (RS1010) on a 1-pin resistor ladder
* **AC Solid State Relay:** Omron G3MB-202P 5V Low-Level Trigger SSR
* **Hardware Interlock Relay:** AEDIKO 24V 1-Channel Mechanical Relay Module
* **Blower MOSFET:** NOYITO AOD4184 isolated MOSFET module
* **Exhaust Fan:** Noctua NF-F12 industrialPPC 24V 3000 RPM IP67 PWM
* **Valve Actuator:** MG90S metal-gear micro servo
* **PTC Heater:** 100W 110V AC enclosed PTC air heating element
* **Recirculation Blower:** 24V DC 5015 / 9733 centrifugal radial blower
* **DC-DC Step-Down:** MP1584EN buck converter (24V down to 5.0V)
* **Thermal Protections:** 70 °C manual-reset KSD301 thermostat, 121 °C SF119E thermal fuse
* **Filter Media:** Sukh virgin activated carbon pellets (acid-free)
