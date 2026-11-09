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

const unsigned long POLL_INTERVAL_MS  = 2000UL;               // 2 seconds
const unsigned long COMM_TIMEOUT_MS   = 60000UL;              // 1 minute failsafe cutoff
const unsigned long RECONNECT_INTV_MS = 10000UL;              // 10s between reconnect attempts
const unsigned long DISPLAY_INTERVAL_MS = 250UL;
const unsigned long PURGE_PWM_SAMPLE_TIMEOUT_US = 5000UL;
const float PURGE_PWM_DUTY_THRESHOLD = 0.8f;
const float MIN_REMOTE_TARGET_TEMP = 40.0f;
const float MAX_REMOTE_TARGET_TEMP = 55.0f;

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
Mode lastDetectedRemote = MODE_OFF;
bool u1OverrideActive = false;
float targetChamberTemp = 50.0f;  // Dynamic setpoint for MODE_HI (defaults to 50C)
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

  // If the user physically turned the knob, physical knob reclaims master control!
  if (rawSwitch != lastPhysicalSwitch) {
    lastPhysicalSwitch = rawSwitch;
    u1OverrideActive   = false;
    currentMode        = rawSwitch;
    if (rawSwitch == MODE_HI) {
      targetChamberTemp = 50.0f; // Manual HI mode defaults strictly to 50C
    }
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

bool readNormalizedFanSpeed(JsonVariantConst speedValue, float& normalizedSpeed) {
  if (!isFiniteNumber(speedValue)) return false;

  float speed = speedValue.as<float>();
  if (speed < 0.0f || speed > 100.0f) return false;

  // The U1 currently reports a fractional value, but accepting 0..100 here
  // keeps this integration tolerant of firmware/API versions using percent.
  normalizedSpeed = speed > 1.0f ? speed / 100.0f : speed;
  return true;
}

bool readU1PurgeSignal() {
  // GPIO 35 is connected to the U1 PA8 PWM signal. A single digitalRead()
  // samples an arbitrary point in the PWM cycle and can cause false purges.
  // This assumes a 3.3 V-compatible, active-high PWM waveform. Verify the
  // signal voltage and duty-cycle meaning electrically before deployment.
  unsigned long highTime = pulseIn(
      PIN_U1_PURGE_SENSE, HIGH, PURGE_PWM_SAMPLE_TIMEOUT_US);
  unsigned long lowTime = pulseIn(
      PIN_U1_PURGE_SENSE, LOW, PURGE_PWM_SAMPLE_TIMEOUT_US);

  if (highTime == 0 || lowTime == 0) return false;

  float dutyCycle = (float)highTime / (float)(highTime + lowTime);
  return dutyCycle >= PURGE_PWM_DUTY_THRESHOLD;
}

// Helper to map SnOrca / Klipper Purifier commands directly to our existing Mode enum
Mode decodeU1Mode(JsonObject purifier, bool hwPurgeActive) {
  if (!purifier) return MODE_OFF;

  float exhaustSpeed = 0.0f;
  readNormalizedFanSpeed(purifier["exhaust_fan"]["speed"], exhaustSpeed);
  int mode = purifier["mode"] | 0;
  float desiredTemp = purifier["desired_chamber_temp"] | 0.0f;

  // 1. Explicit Exhaust Dump / End-of-Print Purge
  if (exhaustSpeed >= PURGE_PWM_DUTY_THRESHOLD || hwPurgeActive) {
    return MODE_PURGE;
  }

  // 2. Direct 1-to-1 Mapping of Klipper Purifier Modes to our 5 Profiles
  switch (mode) {
    case 1:
      // Klipper COOL CHAMBER (SnOrca PLA strong cool) -> MODE_LOW
      return MODE_LOW;

    case 2:
      // Klipper PREHEAT CHAMBER (SnOrca ASA/PC heat soak) -> MODE_HI
      if (desiredTemp >= MIN_REMOTE_TARGET_TEMP &&
          desiredTemp <= MAX_REMOTE_TARGET_TEMP) {
        targetChamberTemp = desiredTemp;
      }
      return MODE_HI;

    case 3:
      // Klipper HOT CHAMBER or WEAK COOL
      if (desiredTemp >= MIN_REMOTE_TARGET_TEMP &&
          desiredTemp <= MAX_REMOTE_TARGET_TEMP) {
        // SnOrca ASA/PC Heat Retention -> MODE_HI
        targetChamberTemp = desiredTemp;
        return MODE_HI;
      } else {
        // SnOrca PETG Weak Cool / Gentle Draft -> MODE_MED
        return MODE_MED;
      }

    default:
      if (exhaustSpeed > 0.05f) return MODE_PURGE;
      return MODE_OFF;
  }
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
      JsonObject activeExtruder = (status[activeExtName].is<JsonObject>() &&
                                   isFiniteNumber(status[activeExtName]["temperature"]))
                                  ? status[activeExtName]
                                  : status["extruder"];

      JsonVariant cavityValue = cavity["temperature"];
      JsonVariant nozzleValue = activeExtruder["temperature"];
      JsonVariant bedValue = bed["temperature"];
      JsonVariant zValue = position[2];
      JsonVariant progressValue = virtualSdcard["progress"];
      JsonVariant durationValue = printStats["print_duration"];
      JsonVariant modeValue = purifier["mode"];
      JsonVariant targetValue = purifier["desired_chamber_temp"];
      JsonVariant exhaustSpeedValue = purifier["exhaust_fan"]["speed"];
      JsonVariant innerSpeedValue = purifier["inner_fan"]["speed"];
      const char* stateValue = printStats["state"];

      if (status && cavity && activeExtruder && bed && toolhead && position &&
          virtualSdcard && printStats && purifier &&
          isFiniteNumber(cavityValue) && isFiniteNumber(nozzleValue) &&
          isFiniteNumber(bedValue) && isFiniteNumber(zValue) &&
          isFiniteNumber(progressValue) && isFiniteNumber(durationValue) &&
          isFiniteNumber(modeValue) && isFiniteNumber(targetValue) &&
          isFiniteNumber(exhaustSpeedValue) && isFiniteNumber(innerSpeedValue) &&
          stateValue != nullptr) {
        float parsedCavity = cavityValue.as<float>();
        float parsedNozzle = nozzleValue.as<float>();
        float parsedBed = bedValue.as<float>();
        float parsedZ = zValue.as<float>();
        float parsedProgress = progressValue.as<float>();
        float parsedDuration = durationValue.as<float>();
        float parsedMode = modeValue.as<float>();
        float parsedTarget = targetValue.as<float>();
        float normalizedExhaustSpeed = 0.0f;
        float normalizedInnerSpeed = 0.0f;

        // Reject values outside the physical/API ranges before refreshing the watchdog.
        if (parsedCavity >= 1.0f && parsedCavity <= 85.0f &&
            parsedNozzle >= 0.0f && parsedNozzle <= 400.0f &&
            parsedBed >= 0.0f && parsedBed <= 150.0f &&
            parsedZ >= -10.0f && parsedZ <= 500.0f &&
            parsedProgress >= 0.0f && parsedProgress <= 1.0f &&
            parsedDuration >= 0.0f && parsedDuration <= 4294967295.0f &&
            parsedMode >= 0.0f && parsedMode <= 3.0f &&
            parsedMode == (float)(int)parsedMode &&
            (parsedTarget == 0.0f ||
             (parsedTarget >= MIN_REMOTE_TARGET_TEMP &&
              parsedTarget <= MAX_REMOTE_TARGET_TEMP)) &&
            readNormalizedFanSpeed(exhaustSpeedValue, normalizedExhaustSpeed) &&
            readNormalizedFanSpeed(innerSpeedValue, normalizedInnerSpeed)) {
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

          // Parse the live Snapmaker U1 dual-fan purifier structure. Both
          // values are already normalized before mode arbitration.
          u1PurifierSpeed = max(normalizedExhaustSpeed, normalizedInnerSpeed);

          // Detect hardware purge signal on Pin 5 (PA8) using measured duty.
          bool hwPurgeActive = readU1PurgeSignal();

          // Decode SnOrca / Klipper command directly into our Mode enum
          Mode detectedRemote = decodeU1Mode(purifier, hwPurgeActive);

          // ARBITRATION: Did U1 command a new active state?
          // Automatic purge has no local duration limit: it remains active
          // until U1 reports MODE_OFF. Manual purge similarly remains active
          // until the physical switch leaves MODE_PURGE.
          if (detectedRemote != lastDetectedRemote) {
            lastDetectedRemote = detectedRemote;
            if (detectedRemote != MODE_OFF) {
              u1OverrideActive = true;
              currentMode      = detectedRemote;
            } else if (u1OverrideActive) {
              // U1 went idle/off -> revert back to physical knob position
              u1OverrideActive = false;
              currentMode      = lastPhysicalSwitch;
            }
          }

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

  // 1. PURGE WITH THERMAL COOLDOWN GATE:
  // Purge has no local timer. Its owner (U1 or the physical switch) decides
  // when it ends.
  if (currentMode == MODE_PURGE) {
    // If the physical knob was manually twisted to PURGE, evacuate immediately (bypass cooldown)
    if (!u1OverrideActive) {
      applyActuators(90, 255, false, false);
      return;
    }

    // Remote U1 / End-of-print purge: enforce bed cooldown gate (<= 60C) to prevent warping!
    bool bedSafeToPurge = (bedTemp <= 60.0f);
    if (bedSafeToPurge) {
      // Phase 2: Bed is cool. Open valve (90 deg), blast exhaust 100% outdoors, blower & heater OFF
      applyActuators(90, 255, false, false);
    } else {
      // Phase 1 (Thermal Soak): Chamber stays SEALED (0 deg), blower ON (scrubbing VOCs), heater OFF
      applyActuators(0, 0, true, false);
    }
    return;
  }

  // 2. Normal Mode State Machine (Shared by Physical Knob and U1 Remote Control)
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

      // TELEMETRY GRACE PERIOD:
      // If telemetry dropped (stale for > 3.5s) but within the 60s watchdog window,
      // freeze last commanded heater state rather than evaluating hysteresis on frozen sensor values!
      bool telemetryStale = (now - lastValidPollTime > POLL_INTERVAL_MS + 1500UL);

      if (!telemetryStale) {
        // SOFTWARE OVER-TEMPERATURE CUTOFF: Hard safety limit at 60C
        if (chamberTemp >= 60.0f) {
          shouldHeat = false;
        }
        // PERSISTENT HYSTERESIS around targetChamberTemp (defaults to 50C)
        else if (chamberTemp < (targetChamberTemp - 1.0f)) {
          shouldHeat = true;
        } else if (chamberTemp >= targetChamberTemp) {
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
  if (u1OverrideActive) {
    if (currentMode == MODE_PURGE) {
      if (bedTemp <= 60.0f) {
        display.print("PURGE [AUTO]");
      } else {
        display.print("COOLDOWN");
      }
    } else {
      switch (currentMode) {
        case MODE_LOW:   display.print("AUTO [PLA]");  break;
        case MODE_MED:   display.print("AUTO [PETG]"); break;
        case MODE_HI:    display.print("AUTO [ASA]");  break;
        default:         display.print("AUTO [U1]");   break;
      }
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
  } else if (u1OverrideActive && currentMode == MODE_PURGE) {
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
}