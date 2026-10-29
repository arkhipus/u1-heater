# ESP32 3D Printer Chamber Controller — Complete Engineering & Modeling Reference

This document compiles the complete system design, component selection, wiring diagrams, Klipper/Moonraker REST API integration, operating modes, and firmware code for the Snapmaker U1 active chamber heating and filtration system.

---

## 1. System Overview & Philosophy

The system replaces standalone thermostats (like the W3230) and manual switches with a single unified **ESP32 microcontroller** that acts as the climate controller for the enclosure.

- **Zero Physical Wires into the Printer Controller:** The ESP32 queries the U1's native chamber thermistor (`cavity`) and print state (`print_stats`) over local Wi-Fi via Moonraker's REST API.
- **Hardware-Level Failsafes:** Safety never relies on software. An inline **121°C thermal fuse** (at the heater air throat) and an independent **70°C manual-reset KSD301 thermostat** (in the chamber air) physically break 110V AC heater power if temperatures exceed safe limits.
- **Single-Knob User Interface:** A 5-position rotary switch on the front panel selects operating profiles: `OFF`, `LOW (PLA)`, `MED (PETG)`, `HI (ASA)`, and `PURGE`.
- **Live Visual Dashboard:** A 0.96" SSD1306 OLED display shows real-time temperatures, target setpoints, fan speeds, valve position, and print progress.

---

## 2. Hardware Bill of Materials (BOM)

| Component | Specification / Model | Purpose |
| :--- | :--- | :--- |
| **Brain** | ESP-WROOM-32 (30-pin, USB-C, CP2102) with Screw Terminal Expansion Shield (AITRIP) | Wi-Fi polling, logic, PWM generation, OLED driver |
| **Power Buck Converter** | MP1584EN Mini DC-DC Buck Converter (tuned to 5.00V DC) | Steps 24V supply down to 5.0V for ESP32 and Servo |
| **Exhaust Fan** | Noctua NF-F12 industrialPPC 24V-3000 SP IP67 PWM (120mm) | High static-pressure exhaust pushing through HEPA + Carbon |
| **Recirculation Fan** | 24V DC 2-wire Blower (e.g. 7530 blower) | Pushes air across PTC heater and Sukh carbon pellet bed |
| **Blower Driver** | NOYITO AOD4184 Optocoupler MOSFET Module (40V 50A) | Switches 24V ground for the 2-wire recirculation blower |
| **Heater Relay** | AEDIKO 24V Relay Module with Optocoupler Isolation (or 5V relay / SSR) | Switches 110V AC Live power line to PTC heater |
| **Exhaust Flap Servo** | MG90S Metal-Gear Micro Servo | Actuates butterfly vent valve between 0° (sealed) and 90° (open) |
| **Display** | ELEGOO 0.96" I2C OLED Display (SSD1306, Yellow Top / Blue Bottom) | Live status dashboard |
| **Mode Selector** | RS1010 1-Pole 5-Position Rotary Band Switch (15mm D-Shaft) | User profile selector switch |
| **Heater Element** | 100W–150W 110V AC PTC Air Heating Element | Active heat source |
| **Safety Cutoff 1** | 121°C Thermal Cutoff Fuse (BOJACK SF119E / Microtemp G4A 117°C–121°C) | Local loss-of-airflow meltdown protection at heater |
| **Safety Cutoff 2** | 70°C Manual-Reset NC Thermostat Switch (KSD301 70°C NC) | Whole-chamber runaway over-temperature limit |
| **Air Filtration** | Standard square HEPA filter + 4mm Sukh activated carbon pellets | Dual-stage UFP and VOC air treatment |

---

## 3. The 5 Operating Profiles (Modes)

| Mode | Target Temp | Butterfly Servo | Noctua Exhaust Fan | Recirculation Blower | 110V PTC Heater | Purpose |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`OFF`** | None | **0° (Closed)** | **0%** | **0%** | **FORCE OFF** | Silent standby. OLED turns off after 30 sec |
| **`LOW` (PLA / TPU)** | < 32°C | **90° (Wide Open)** | **70%** | **0%** | **FORCE OFF** | Exhausts heat out of top cover to prevent heat creep |
| **`MED` (PETG)** | ~38°C | **90° (Wide Open)** | **15% (Gentle draft)** | **OFF (or ON for odor scrub)** | **FORCE OFF** | Maintains moderate temp, gentle carbon odor filtering |
| **`HI` (ASA / PC)** | **50°C – 55°C** | **0° (Sealed airtight)** | **0%** | **100% (Full blast)** | **ENABLED (PID/Hysteresis)** | Enclosed baking oven. Recirculation scrubs VOCs |
| **`PURGE`** | Flush room | **90° (Wide Open)** | **100% (Full blast)** | **0%** | **FORCE OFF** | Instant room/chamber air flush through HEPA + Carbon |

---

## 4. Aerodynamic & Filtration Chain

The exhaust duct stack is assembled in positive-pressure order:

```text
[ Chamber Interior ]
        │
        ▼
[ Butterfly Flap Valve ]  <-- Controlled by MG90S Servo (0° = Sealed, 90° = Open)
        │
        ▼
[ Noctua NF-F12 Fan ]     <-- Pushes air forward into the filter housing
        │
        ▼
[ HEPA Filter ]           <-- Catches UFPs and dust FIRST so carbon pores don't clog
        │
        ▼
[ Sukh Carbon Pellet Bed] <-- Adsorbs gaseous VOCs (Styrene) from clean particulate-free air
        │
        ▼
[ Exhaust Outlet / Room ]
```

### The Binary Valve Rule
- When Exhaust Fan is **0%**: Valve is **0° (100% CLOSED)** for an airtight thermal seal.
- When Exhaust Fan is **> 0%**: Valve is **90° (100% WIDE OPEN)** to prevent fan blade stall and turbulence against the high-resistance filter stack. Flow rate is modulated purely by fan PWM.

---

## 5. Electrical Wiring & Schematics

### 5.1 Power Distribution Tree

```text
Snapmaker U1 Top Port (Molex Micro-Fit 3.0 2×3)
├── Pin 6 (+24V Fused 0.9A) ──> MP1584 Buck (5V) & 24V Fan/Blower
├── Pin 2 (GND) ──────────────> System Common Ground Bus
├── Pin 4 (FLAG2) ────────────> 10kΩ Resistor to Pin 2 (GND) [Wakes up U1 Purifier]
└── Pin 5 (PA8 PWM) ──────────> GPIO 35 [Hardware Exhaust/Purge Sense]
```

```text
[ 24V DC Main Supply ]
       │
       ├─────────────────────────────────► NOYITO MOSFET Power (+) [24V for Blower]
       │
       ├─────────────────────────────────► Noctua NF-F12 Yellow Wire (+24V)
       │
       └─► [ MP1584 Buck Converter ] ──► Tuned to 5.00V DC Output
                 │
                 ├──► ESP32 "VIN" / "V5" Terminal
                 │
                 └──► MG90S Servo RED Wire (+5V Power)

[ COMMON GROUND BUS (GND) ]
       Connect together:
       - 24V Power Supply GND
       - MP1584 Buck Converter OUT- (GND)
       - ESP32 GND Terminal
       - Noctua Black Wire (GND)
       - MG90S Brown Wire (GND)
       - NOYITO MOSFET Signal GND
```

### 5.2 110V AC Heater Safety Chain (Isolated from Logic)
```text
[ 110V AC Live (Black) ]
       │
       ▼
[ Relay COM ➔ NO Terminals ]  <-- Controlled by ESP32 via Optocoupler Relay
       │
       ▼
[ KSD301 70°C NC Switch ]     <-- Chamber ceiling manual-reset cutoff
       │
       ▼
[ 121°C Thermal Fuse (SF119E)]<-- Local heater intake/throat cutoff
       │
       ├───► [ PTC Heating Element (110V AC) ] ───┐
       │                                          │
       └───► [ Neon / Pilot Light Indicator ] ────┤
                                                  │
[ 110V AC Neutral (White) ] ──────────────────────┴──► To Wall Neutral
```
*Notice: Pilot light is wired directly across the PTC element terminals downstream of all fuses. If any safety fuse trips, the light goes dark.*

### 5.3 1-Pin Resistor Ladder for 5-Position Switch
Connect across the RS1010 rotary switch terminals:
```text
       ESP32 3.3V Pin
            │
            ├───► Pin 5 (PURGE) ──────► 3.30V (Direct to 3.3V)
            │
        [R1: 10k]
            │
            ├───► Pin 4 (HI)    ──────► 2.48V (3/4 of 3.3V)
            │
        [R2: 10k]
            │
            ├───► Pin 3 (MED)   ──────► 1.65V (1/2 of 3.3V)
            │
        [R3: 10k]
            │
            ├───► Pin 2 (LOW)   ──────► 0.82V (1/4 of 3.3V)
            │
        [R4: 10k]
            │
           GND (0V)
```
- **Pin 1 (Common Wiper):** Connects to **`GPIO 34`** on the ESP32.
- **Pull-down:** Connect a **100kΩ resistor** between `GPIO 34` and **GND**.
  - When switch is in Position 1 (`OFF`), Pin 1 touches nothing; the 100kΩ resistor pulls `GPIO 34` to **0.00V**.

### 5.4 ESP32 Pin Allocation Table (30-Pin NodeMCU)

| Pin Name | Connected Device | Voltage Level | Notes |
| :--- | :--- | :--- | :--- |
| **`VIN` / `5V`** | MP1584 Buck Converter (+5.0V) | 5.0V Power Input | Feeds onboard 3.3V LDO regulator |
| **`GND`** | Common Ground Bus | 0V | Tied to 24V GND, 5V GND, Servo GND |
| **`3V3`** | OLED `VCC`, Top of Switch Ladder | 3.3V Power Output | Up to 150mA clean sensor power |
| **`GPIO 21`** | OLED `SDA` (I2C Data) | 3.3V Logic | Standard ESP32 I2C Data |
| **`GPIO 22`** | OLED `SCL` (I2C Clock) | 3.3V Logic | Standard ESP32 I2C Clock |
| **`GPIO 18`** | MG90S Servo Signal (Yellow) | 3.3V PWM Logic | Uses `ESP32Servo` library |
| **`GPIO 19`** | Noctua NF-F12 Blue Wire (PWM) | 3.3V PWM Logic | 25 kHz PWM speed control |
| **`GPIO 23`** | NOYITO MOSFET `IN+` (Blower) | 3.3V / 5V Logic | Drives optocoupler gate |
| **`GPIO 25`** | Heater Relay Board `IN` | 3.3V / 5V Logic | Controls 110V Live AC relay |
| **`GPIO 34`** | 5-Way Switch Common Wiper | 0.0V – 3.3V Analog | Input-only ADC1 pin |

---

### 5.5 Physical Connectors Pinout on Heater Box

#### Connector A: 8-Pin Low-Voltage DC & Logic (Cleanly Grouped!)
Pins 1–4 branch to the Exhaust & Servo assembly; Pins 4–8 branch to the Front Panel:
1. **Pin 1:** `+24V DC` (Feeds Noctua fan & local 5V buck converter for servo)
2. **Pin 2:** `Servo PWM` (GPIO 18)
3. **Pin 3:** `Exhaust Fan PWM` (GPIO 19 to Noctua Blue wire)
4. **Pin 4:** `GND` (Common ground bus)
5. **Pin 5:** `+3.3V DC` (Clean 3.3V logic supply for OLED & switch ladder)
6. **Pin 6:** `Display SCL` (GPIO 22)
7. **Pin 7:** `Display SDA` (GPIO 21)
8. **Pin 8:** `Mode Selector` (GPIO 34 ADC)

#### Connector B: 5-Pin 110V AC Mains & Safety Loop (100% High Voltage Isolated!)
1. **Pin 1:** `110V AC Live (L)` (Black wire in from wall plug)
2. **Pin 2:** `110V AC Neutral (N)` (White wire in from wall plug)
3. **Pin 3:** `Earth Ground (PE)` (Green wire to metal chassis / safety ground)
4. **Pin 4:** `KSD301 Loop Out` (110V Live series wire to chamber switch)
5. **Pin 5:** `KSD301 Loop Return` (110V Live returning from chamber switch)

---

## 6. Moonraker REST API Reference

The ESP32 queries Moonraker using a single HTTP GET request every 2000 ms:

```http
GET http://<U1-IP-Address>/printer/objects/query?print_stats&virtual_sdcard&temperature_sensor%20cavity&purifier
```

### Response JSON Structure:
```json
{
  "result": {
    "status": {
      "temperature_sensor cavity": {
        "temperature": 50.2
      },
      "extruder": {
        "temperature": 260.0
      },
      "heater_bed": {
        "temperature": 100.0
      },
      "toolhead": {
        "position": [120.0, 110.0, 18.4, 45.2],
        "extruder": "extruder1"
      },
      "virtual_sdcard": {
        "progress": 0.74
      },
      "print_stats": {
        "state": "printing",
        "print_duration": 6120
      },
      "purifier": {
        "speed": 0.0,
        "is_enabled": false
      }
    }
  }
}
```

### State Machine Transitions & Autonomous Purge Detection:
- **Automatic Purge Detection (Overrides Any Knob Position):** When the U1 runs its post-print purge routine (or when SnOrca/Klipper commands the exhaust fan), Moonraker reports `purifier.speed > 0` (or `purifier.is_enabled == true`).
  - The ESP32 immediately enters **Purge Mode**: opens the butterfly valve to 90°, ramps the Noctua exhaust fan, and kills the heater.
  - The moment the U1 finishes its purge cycle and sets `purifier.speed == 0`, the ESP32 automatically closes the valve and returns to the knob's selected state!
  - You do not need hardcoded timers on the ESP32: the ESP32 follows the U1's start and stop precisely.
- `state == "printing"` ➔ Active print. Regulate chamber according to selected rotary knob mode.
- `state == "standby" / "paused"` ➔ Idle state.
- **Watchdog Timeout:** If no valid HTTP 200 response is received for **> 10 seconds** (or Wi-Fi disconnects), the ESP32 immediately cuts the heater by driving `PIN_HEATER_RELAY` `HIGH` (Active-LOW SSR cutoff) and displays a telemetry warning.

---

## 7. Complete ESP32 Firmware Source Code

```cpp
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>
#include "secrets.h"

// --- Pin Definitions ---
#define PIN_SERVO          18
#define PIN_NOCTUA_PWM     19
#define PIN_BLOWER         23
#define PIN_HEATER_RELAY   25  // Active-LOW SSR: LOW=ON, HIGH=OFF
#define PIN_SWITCH_ADC     34
#define PIN_U1_PURGE_SENSE 35  // Hardware PWM input from U1 Top Port Pin 5 (PA8)
#define PIN_I2C_SDA        21
#define PIN_I2C_SCL        22

// --- Constants ---
#define SCREEN_WIDTH      128
#define SCREEN_HEIGHT     64
#define OLED_RESET        -1
#define OLED_I2C_ADDR     0x3C

const unsigned long PURGE_DURATION_MS = 10UL * 60UL * 1000UL; // 10 minutes
const unsigned long POLL_INTERVAL_MS  = 2000UL;               // 2 seconds
const unsigned long COMM_TIMEOUT_MS   = 60000UL;              // 1 minute failsafe cutoff
const unsigned long RECONNECT_INTV_MS = 10000UL;              // 10s between reconnect attempts
const unsigned long DISPLAY_INTERVAL_MS = 250UL;

// --- State Enums ---
enum Mode { MODE_OFF, MODE_LOW, MODE_MED, MODE_HI, MODE_PURGE };

// --- Globals ---
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Servo flapServo;

// Actuator State Tracking
int currentServoAngle = 0;
int currentNoctuaPWM  = 0;
bool currentBlowerOn  = false;
bool currentHeaterOn  = false;

// Arbitration: "Last Action Wins" State
Mode currentMode = MODE_OFF;
Mode lastPhysicalSwitch = MODE_OFF;
bool u1OverrideActive = false;
float lastU1Speed = 0.0f;
bool telemetryWatchdogOk = false; // Latch: True once valid telemetry received, false if watchdog expires
bool telemetryLost = false;       // True when the most recent poll failed (grace period in progress)
float chamberTemp = 0.0;
float nozzleTemp = 0.0;
float bedTemp = 0.0;
float toolheadZ = 0.0;
String activeTool = "T0";
float printProgress = 0.0;
unsigned long printDuration = 0;
String printState = "offline";
float u1PurifierSpeed = 0.0;
bool u1PurifierEnabled = false;
bool u1IsPurging = false;

// Timers
unsigned long lastPollTime = 0;
unsigned long lastValidPollTime = 0;
unsigned long lastDisplayTime = 0;
unsigned long lastReconnectAttempt = 0;

void setup() {
  Serial.begin(115200);

  // Initialize Outputs (Heater starts OFF: HIGH for active-low SSR)
  pinMode(PIN_HEATER_RELAY, OUTPUT);
  digitalWrite(PIN_HEATER_RELAY, HIGH); 
  pinMode(PIN_BLOWER, OUTPUT);
  pinMode(PIN_NOCTUA_PWM, OUTPUT);
  pinMode(PIN_U1_PURGE_SENSE, INPUT);
  digitalWrite(PIN_BLOWER, LOW);
  analogWrite(PIN_NOCTUA_PWM, 0);

  // Initialize Servo
  flapServo.attach(PIN_SERVO);
  flapServo.write(0); // Sealed default

  // Initialize I2C and OLED
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    // Top Yellow Zone (Y=0..15): place cleanly at Y=4
    display.setCursor(0, 4);
    display.println("Connecting WiFi...");
    display.drawLine(0, 14, 128, 14, SSD1306_WHITE);

    // Blue Zone (Y=16..63)
    display.setCursor(0, 24);
    display.print("SSID: ");
    display.println(WIFI_SSID);
    display.display();
  }

  // Initialize WiFi
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Give WiFi up to 10 seconds to connect during boot
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi pending, moving to main loop...");
  }
}

void loop() {
  unsigned long now = millis();

  // ========================================================
  // 1. WI-FI DISCONNECT HANDLING (AUTO-RESUMING)
  // ========================================================
  if (WiFi.status() != WL_CONNECTED) {
    handleWifiDisconnected(now);
    return; // Loops here trying to reconnect; once back online, loop continues!
  }

  // ========================================================
  // 2. NORMAL RUNNING LOOP (WI-FI CONNECTED)
  // ========================================================
  // --- ARBITRATION: Last Action Wins ---
  Mode rawSwitch = readSwitchMode();

  // Event 1: Did the user physically turn the knob?
  if (rawSwitch != lastPhysicalSwitch) {
    lastPhysicalSwitch = rawSwitch;
    u1OverrideActive = false; // Physical knob immediately reclaims control!
    currentMode = rawSwitch;
  }

  // Event 2: Did U1 hardware or Moonraker send a purge/vent command?
  if (u1IsPurging) {
    u1OverrideActive = true; // U1 touchscreen/macro takes control!
  } else if (u1OverrideActive && !u1IsPurging) {
    // U1 finished venting -> revert back to physical knob setting
    u1OverrideActive = false;
    currentMode = lastPhysicalSwitch;
  }

  // Poll Moonraker API every 2 seconds (or immediately on first loop pass)
  if (lastPollTime == 0 || now - lastPollTime >= POLL_INTERVAL_MS) {
    lastPollTime = now;
    pollMoonraker();
  }

  // Check 1-minute safety timeout on telemetry
  // If no valid packet has ever arrived (lastValidPollTime == 0), or if > 60s since last valid packet:
  bool commsTimeout = (lastValidPollTime == 0) || (millis() - lastValidPollTime >= COMM_TIMEOUT_MS);

  if (commsTimeout) {
    // Safety Cutoff: Suspend heater while telemetry is missing/stale, but keep running loop
    digitalWrite(PIN_HEATER_RELAY, HIGH); // Active-LOW: HIGH = OFF
    currentHeaterOn = false;
    telemetryWatchdogOk = false;
  } else {
    // Telemetry watchdog is healthy: Execute normal environmental control logic
    executeChamberLogic(now);
  }

  // Throttle OLED I2C bus traffic to DISPLAY_INTERVAL_MS (250 ms = 4 FPS)
  if (now - lastDisplayTime >= DISPLAY_INTERVAL_MS) {
    lastDisplayTime = now;
    if (commsTimeout) {
      displayTimeoutWarning(now);
    } else {
      updateDisplay();
    }
  }
}

void handleWifiDisconnected(unsigned long now) {
  // Safe elapsed calculation since last valid telemetry
  unsigned long elapsedWithoutData = (lastValidPollTime > 0) ? (now - lastValidPollTime) : now;
  bool timeoutExceeded = (elapsedWithoutData >= COMM_TIMEOUT_MS);

  if (timeoutExceeded) {
    // 60-SECOND WATCHDOG CUTOFF: Wi-Fi down too long -> cut heater
    digitalWrite(PIN_HEATER_RELAY, HIGH); // Active-LOW: HIGH = OFF
    currentHeaterOn = false;
    telemetryWatchdogOk = false;
  }
  // Within the 60s grace period:
  // Retain last known actuator states (heats through temporary Wi-Fi hiccups)

  // Display "NO WI-FI" screen with status and live countdown
  display.clearDisplay();
  
  // Yellow Header (Rows 0-15)
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("NETWORK: [RECONNECT]");
  display.drawLine(0, 12, 128, 12, SSD1306_WHITE);

  // Blue Main Body (Rows 16-63)
  display.setCursor(14, 20);
  display.setTextSize(2);
  display.print("NO WI-FI");

  display.setTextSize(1);
  display.setCursor(0, 42);
  if (timeoutExceeded) {
    display.print("HEAT SUSPENDED (SAFE)");
    display.setCursor(0, 53);
    display.print("WAITING TO RESUME...");
  } else {
    display.print(currentHeaterOn ? "HEATING (GRACE PERIOD)" : "RECONNECTING...");
    display.setCursor(0, 53);
    unsigned long secLeft = (COMM_TIMEOUT_MS - elapsedWithoutData) / 1000;
    display.print("Cutoff in: ");
    display.print(secLeft);
    display.print("s");
  }
  display.display();

  // Retry reconnecting every 10 seconds (don't hammer the radio!)
  if (now - lastReconnectAttempt >= RECONNECT_INTV_MS) {
    lastReconnectAttempt = now;
    Serial.println("Attempting WiFi reconnect...");
    WiFi.reconnect();
  }

  delay(250);
}

void displayTimeoutWarning(unsigned long now) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("TELEMETRY: [WAITING]");
  display.drawLine(0, 12, 128, 12, SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 20);
  if (lastValidPollTime == 0) {
    display.print("WAITING FOR U1 DATA");
  } else {
    display.print("NO TELEMETRY > 1 MIN");
  }
  display.setCursor(0, 34);
  display.print("HEATER: [SUSPENDED]");
  display.setCursor(0, 48);
  display.print("AUTO-RESUMES ON DATA");
  display.display();
}

Mode readSwitchMode() {
  int rawADC = analogRead(PIN_SWITCH_ADC); // 0 to 4095
  // Voltage bands for 5 positions (approx 0V, 0.82V, 1.65V, 2.48V, 3.3V)
  if (rawADC < 400)  return MODE_OFF;    // ~0.0V
  if (rawADC < 1500) return MODE_LOW;    // ~0.82V
  if (rawADC < 2500) return MODE_MED;    // ~1.65V
  if (rawADC < 3500) return MODE_HI;     // ~2.48V
  if (rawADC <= 4095) return MODE_PURGE; // ~3.3V
  return MODE_OFF;                       // Invalid ADC reading: fail safe
}

bool isFiniteNumber(JsonVariantConst value) {
  if (!value.is<float>() && !value.is<double>() &&
      !value.is<int>() && !value.is<unsigned int>() &&
      !value.is<long>() && !value.is<unsigned long>()) {
    return false;
  }
  return isfinite(value.as<float>());
}

void pollMoonraker() {
  HTTPClient http;
  String url = "http://" + String(U1_IP_ADDRESS) + 
               "/printer/objects/query?print_stats&virtual_sdcard&temperature_sensor%20cavity&purifier&heater_bed&extruder&extruder1&toolhead";
  http.begin(url);
  http.setTimeout(1500);

  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      JsonObject status = doc["result"]["status"];
      JsonObject cavity = status["temperature_sensor cavity"];
      JsonObject bed = status["heater_bed"];
      JsonObject toolhead = status["toolhead"];
      JsonArray position = toolhead["position"];
      JsonObject virtualSdcard = status["virtual_sdcard"];
      JsonObject printStats = status["print_stats"];
      JsonObject purifier = status["purifier"];

      const char* extruderName = toolhead["extruder"];
      String activeExtName = (extruderName != nullptr) ? String(extruderName) : "extruder";

      // Dynamically select the ACTIVE extruder based on toolhead (T0 vs T1)
      JsonObject activeExtruder = (status.containsKey(activeExtName) && !status[activeExtName]["temperature"].isNull())
                                  ? status[activeExtName]
                                  : status["extruder"];

      JsonVariant cavityValue = cavity["temperature"];
      JsonVariant nozzleValue = activeExtruder["temperature"];
      JsonVariant bedValue = bed["temperature"];
      JsonVariant zValue = position[2];
      JsonVariant progressValue = virtualSdcard["progress"];
      JsonVariant durationValue = printStats["print_duration"];
      const char* stateValue = printStats["state"];

      if (status && cavity && activeExtruder && bed && toolhead && position &&
          virtualSdcard && printStats &&
          isFiniteNumber(cavityValue) && isFiniteNumber(nozzleValue) &&
          isFiniteNumber(bedValue) && isFiniteNumber(zValue) &&
          isFiniteNumber(progressValue) && isFiniteNumber(durationValue) &&
          stateValue != nullptr) {
        float parsedCavity = cavityValue.as<float>();
        float parsedNozzle = nozzleValue.as<float>();
        float parsedBed = bedValue.as<float>();
        float parsedZ = zValue.as<float>();
        float parsedProgress = progressValue.as<float>();
        float parsedDuration = durationValue.as<float>();

        // Reject values outside the physical/API ranges before refreshing the watchdog.
        if (parsedCavity >= 1.0f && parsedCavity <= 85.0f &&
            parsedNozzle >= 0.0f && parsedNozzle <= 400.0f &&
            parsedBed >= 0.0f && parsedBed <= 150.0f &&
            parsedZ >= -10.0f && parsedZ <= 500.0f &&
            parsedProgress >= 0.0f && parsedProgress <= 1.0f &&
            parsedDuration >= 0.0f && parsedDuration <= 4294967295.0f) {
          chamberTemp = parsedCavity;
          nozzleTemp  = parsedNozzle; // Live temperature of the active nozzle!
          bedTemp     = parsedBed;
          toolheadZ   = parsedZ;

          // Toolhead identification
          if (activeExtName == "extruder1") {
            activeTool = "T1";
          } else if (activeExtName == "extruder2") {
            activeTool = "T2";
          } else if (activeExtName == "extruder3") {
            activeTool = "T3";
          } else {
            activeTool = "T0";
          }

          printProgress = parsedProgress;
          printState    = stateValue;
          printDuration = (unsigned long)parsedDuration;

          // Parse Real Snapmaker U1 Dual-Fan Purifier structure:
          float exhaustSpeed = 0.0f;
          float innerSpeed   = 0.0f;
          if (purifier && purifier.containsKey("exhaust_fan")) {
            exhaustSpeed = purifier["exhaust_fan"]["speed"] | 0.0f;
          }
          if (purifier && purifier.containsKey("inner_fan")) {
            innerSpeed = purifier["inner_fan"]["speed"] | 0.0f;
          }
          int purifierMode = purifier ? (purifier["mode"] | 0) : 0;

          u1PurifierSpeed = max(exhaustSpeed, innerSpeed);
          if (u1PurifierSpeed > 1.0f) u1PurifierSpeed /= 100.0f; // normalize 0-100 scale

          // Detect purge via API or Hardware Pin 5 (PA8)
          bool hwPurgeActive = (digitalRead(PIN_U1_PURGE_SENSE) == HIGH); // or PWM active
          u1IsPurging = (exhaustSpeed > 0.05f) || (innerSpeed > 0.05f) || (purifierMode > 0) || hwPurgeActive;

          // Success: Refresh watchdog and mark telemetry watchdog OK
          lastValidPollTime   = millis();
          telemetryWatchdogOk = true;
          telemetryLost       = false;
          http.end();
          return;
        }
      }
    }
  }

  // If HTTP failed, JSON was corrupt, or sensor was missing/insane:
  http.end();
  telemetryLost = true;
  // Do NOT update lastValidPollTime!
  // If stale for > COMM_TIMEOUT_MS, loop() will cut heater.
}

void executeChamberLogic(unsigned long now) {
  // CRITICAL FAIL-SAFE: If telemetry has never initialized or watchdog expired, heater MUST be off!
  if (!telemetryWatchdogOk) {
    applyActuators(currentServoAngle, currentNoctuaPWM, currentBlowerOn, false);
    return;
  }

  // 1. U1 OVERRIDE (Touchscreen / G-code / Purge) & COOLDOWN GATE:
  if (u1OverrideActive || currentMode == MODE_PURGE) {
    // Bed must cool to <= 60C before opening exhaust outdoors to prevent warping!
    bool bedSafeToPurge = (bedTemp <= 60.0f);

    // If manual knob is explicitly set to PURGE, override cooldown immediately
    if (currentMode == MODE_PURGE) {
      applyActuators(90, 255, false, false);
      return;
    }

    if (bedSafeToPurge) {
      // Phase 2: Bed is cool. Open valve (90 deg), blast exhaust 100% outdoors, blower & heater OFF
      applyActuators(90, 255, false, false);
    } else {
      // Phase 1 (Thermal Soak): Chamber stays SEALED (0 deg), blower ON (scrubbing VOCs), heater OFF
      applyActuators(0, 0, true, false);
    }
    return;
  }

  // 2. Normal Mode State Machine
  switch (currentMode) {
    case MODE_OFF:
      applyActuators(0, 0, false, false);
      break;

    case MODE_LOW: // PLA: Flap wide open, exhaust cooling active
      applyActuators(90, 180, false, false); // Valve 90, Noctua ~70%, Blower OFF, Heat OFF
      break;

    case MODE_MED: // PETG: Flap open, gentle exhaust draft
      applyActuators(90, 40, false, false);  // Valve 90, Noctua ~15%, Blower OFF, Heat OFF
      break;

    case MODE_HI: { // ASA/PC: Sealed chamber, 100% recirculation, active heat
      bool shouldHeat = currentHeaterOn; // Default: retain previous state (hysteresis)

      // TELEMETRY GRACE PERIOD (Cleanup Step 3):
      // If telemetry dropped (stale for > 3.5s) but within the 60s watchdog window,
      // freeze last commanded heater state rather than evaluating hysteresis on frozen sensor values!
      bool telemetryStale = (now - lastValidPollTime > POLL_INTERVAL_MS + 1500UL);

      if (!telemetryStale) {
        // SOFTWARE OVER-TEMPERATURE CUTOFF: Hard limit at 60C
        if (chamberTemp >= 60.0f) {
          shouldHeat = false;
        }
        // PERSISTENT HYSTERESIS: Target 50C (ON below 49.0C, OFF at or above 50.0C)
        else if (chamberTemp < 49.0f) {
          shouldHeat = true;
        } else if (chamberTemp >= 50.0f) {
          shouldHeat = false;
        }
      }

      applyActuators(0, 0, true, shouldHeat); // Valve 0, Noctua 0, Blower ON, Heat controlled
      break;
    }

    case MODE_PURGE:
      applyActuators(90, 255, false, false);
      break;
  }
}

void applyActuators(int servoAngle, int noctuaPWM, bool blowerOn, bool heatEnable) {
  // SAFETY INTERLOCK: Never run exhaust fan against a closed butterfly valve!
  noctuaPWM = constrain(noctuaPWM, 0, 255);
  if (noctuaPWM > 0) {
    servoAngle = 90; // Any exhaust flow requires the valve fully open
  } else {
    servoAngle = 0;  // No exhaust flow permits the sealed position
  }

  // Save current commanded state for rock-solid OLED display
  currentServoAngle = servoAngle;
  currentNoctuaPWM  = noctuaPWM;
  currentBlowerOn   = blowerOn;
  currentHeaterOn   = heatEnable;

  // Send to hardware
  flapServo.write(servoAngle);
  analogWrite(PIN_NOCTUA_PWM, noctuaPWM);
  digitalWrite(PIN_BLOWER, blowerOn ? HIGH : LOW);
  // Active-LOW SSR: LOW = ON (Heater energized), HIGH = OFF (Heater dead)
  digitalWrite(PIN_HEATER_RELAY, heatEnable ? LOW : HIGH);
}

// Helper to format seconds cleanly as "30:23" (MM:SS or HH:MM) in exactly 5 characters
void printColonTime(unsigned long totalSecs) {
  unsigned long hrs = totalSecs / 3600;
  unsigned long mins = (totalSecs % 3600) / 60;
  unsigned long secs = totalSecs % 60;

  if (hrs > 0) {
    if (hrs < 10) display.print("0");
    display.print(hrs);
    display.print(":");
    if (mins < 10) display.print("0");
    display.print(mins);
  } else {
    if (mins < 10) display.print("0");
    display.print(mins);
    display.print(":");
    if (secs < 10) display.print("0");
    display.print(secs);
  }
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextWrap(false); // Prevents accidental character wrapping/line drop

  // ========================================================
  // 1. TOP YELLOW BAND (Rows 0 - 15): Mode & Graphical Progress
  // ========================================================
  display.setTextSize(1);
  display.setCursor(0, 2);
  if (u1IsPurging) {
    if (bedTemp <= 60.0f) {
      display.print("PURGE [AUTO]");
    } else {
      display.print("COOLDOWN");
    }
  } else {
    switch (currentMode) {
      case MODE_OFF:   display.print("OFF");        break;
      case MODE_LOW:   display.print("LOW [PLA]");  break;
      case MODE_MED:   display.print("MED [PETG]"); break;
      case MODE_HI:    display.print("HIGH [ASA]"); break;
      case MODE_PURGE: display.print("PURGE [MAN]");break;
    }
  }

  // Progress Percentage & Mini Graphical Progress Bar on Top Right
  if (printState == "printing") {
    int pct = (int)(printProgress * 100);
    display.setCursor(74, 2);
    if (pct < 10) display.print(" ");
    display.print(pct);
    display.print("%");

    // Battery-style progress box at X=102, Y=2, W=25, H=7
    display.drawRect(102, 2, 25, 7, SSD1306_WHITE);
    int fillW = map(pct, 0, 100, 0, 21);
    if (fillW > 0) {
      display.fillRect(104, 4, fillW, 3, SSD1306_WHITE);
    }
  } else {
    display.setCursor(105, 2);
    display.print(WiFi.status() == WL_CONNECTED ? "WiFi" : "--");
  }

  // Horizontal divider separating yellow and blue zones
  display.drawLine(0, 12, 128, 12, SSD1306_WHITE);

  // ========================================================
  // 2. BLUE SECTION (Rows 16 - 63): Shifted 1 Char Left (X=62)
  // ========================================================

  // Line 1 (Y = 16): Chamber Temp & PTC Heater Status
  display.setCursor(0, 16);
  display.print("CHMB:");
  display.print(chamberTemp, 1);
  display.print("C");

  display.setCursor(62, 16);
  display.print("| HTR: ");
  // Active-LOW relay check: LOW = ON, HIGH = OFF (no trailing space to prevent wrap)
  display.print(currentHeaterOn ? "ON" : "OFF");

  // Line 2 (Y = 27): Bed Temp & Nozzle Temp
  display.setCursor(0, 27);
  display.print("BED: ");
  display.print((int)bedTemp);
  display.print("C");

  display.setCursor(62, 27);
  display.print("| NOZ: ");
  display.print((int)nozzleTemp);
  display.print("C");

  // Line 3 (Y = 38): Butterfly Valve Status & Exhaust Fan Speed %
  display.setCursor(0, 38);
  display.print("VLV:");
  display.print(currentServoAngle == 0 ? "CLOSED" : "OPEN  ");

  display.setCursor(62, 38);
  display.print("| EXH: ");
  int exhPct = map(currentNoctuaPWM, 0, 255, 0, 100);
  if (exhPct < 10) display.print(" ");
  display.print(exhPct);
  display.print("%");

  // Line 4 (Y = 49): Telemetry Grace Countdown, Print Time/ETA, Cooldown Soak, or Purge Banner
  display.setCursor(0, 49);
  if (telemetryLost) {
    // Telemetry dropped but still inside the 60s grace period: warn and count down
    // to the heater suspension instead of drawing the now-stale TME/ETA values.
    unsigned long elapsedWithoutData = millis() - lastValidPollTime;
    unsigned long secLeft = (elapsedWithoutData < COMM_TIMEOUT_MS)
                              ? (COMM_TIMEOUT_MS - elapsedWithoutData) / 1000
                              : 0;
    display.print("TELEM LOST");
    display.setCursor(62, 49);
    display.print("| CUT:");
    if (secLeft < 10) display.print(" ");
    display.print(secLeft);
    display.print("s");
  } else if (u1IsPurging) {
    if (bedTemp <= 60.0f) {
      display.print("U1 PURGE IN PROGRESS");
    } else {
      // Cooldown soak banner: e.g. "SOAK: BED 85C->60C" (fits cleanly in 128px)
      display.print("SOAK: BED ");
      display.print((int)bedTemp);
      display.print("C->60C");
    }
  } else if (printState == "printing") {
    display.print("TME:");
    printColonTime(printDuration);

    display.setCursor(62, 49);
    display.print("| ETA:");
    if (printProgress > 0.02) {
      unsigned long totalEst = (unsigned long)(printDuration / printProgress);
      unsigned long rem = (totalEst > printDuration) ? (totalEst - printDuration) : 0;
      printColonTime(rem);
    } else {
      display.print(" -- ");
    }
  } else {
    display.print("STATUS: ");
    display.print(printState);
  }
  display.display();
}```

---

## 8. Physical Modeling & Enclosure Design Guidelines (For Fusion 360)

When modeling the physical housing for this controller, incorporate these exact cutouts and clearances:

1. **OLED Screen Cutout (0.96" SSD1306):**
   - Visible Glass Window: **23.5 mm × 13.0 mm**.
   - PCB Outer Outline: **27.5 mm × 27.5 mm** with 4 corner mounting holes spaced **22.0 mm × 22.0 mm** (for M2 or M2.5 screws).
2. **Rotary Switch Cutout (RS1010):**
   - Mounting Bushing Hole: **9.0 mm diameter**.
   - Include a small **2.0 mm anti-rotation locator hole** spaced 7.5 mm from center (matches the locking tab on the switch body).
   - Shaft Length: 15 mm D-shaft (accepts standard 20 mm skirted aluminum volume knobs).
3. **AITRIP ESP32 Terminal Shield Mount:**
   - PCB Footprint: **77.5 mm × 63.0 mm**.
   - 4 corner standoff holes: spaced **71.0 mm × 56.5 mm** (M3 heat-set inserts or standoffs).
4. **MP1584 Buck Converter Mount:**
   - Dimensions: **22.0 mm × 17.0 mm**.
   - Model a small rectangular drop-in pocket with snap clips or side zip-tie tie-down holes.
5. **Servo Butterfly Flap Bracket:**
   - Mount the MG90S servo with its output gear spline concentric with or parallel to the butterfly valve's pivot axis.
   - Use a 1.0 mm steel pushrod with Z-bends connecting the servo horn to the pull lever.
   - Design mechanical stop ribs at **0°** and **90°** so the servo linkage cannot force the valve past its travel limit.
6. **Mains High-Voltage Partition:**
   - Keep the 110V AC power terminal block and the relay output completely compartmentalized from the low-voltage ESP32 and sensor wiring. Maintain at least **15 mm air gap** between AC mains conductors and low-voltage logic lines.
