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
#define PIN_SERVO         18
#define PIN_NOCTUA_PWM    19
#define PIN_BLOWER        23
#define PIN_HEATER_RELAY  25  // Active-LOW SSR: LOW=ON, HIGH=OFF
#define PIN_SWITCH_ADC    34
#define PIN_I2C_SDA       21
#define PIN_I2C_SCL       22

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

// System & Telemetry State
Mode currentMode = MODE_OFF;
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
  // Read Physical 5-Way Rotary Switch
  currentMode = readSwitchMode();

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
               "/printer/objects/query?print_stats&virtual_sdcard&temperature_sensor%20cavity&purifier&heater_bed&extruder&toolhead";
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
      JsonObject extruder = status["extruder"];
      JsonObject bed = status["heater_bed"];
      JsonObject toolhead = status["toolhead"];
      JsonArray position = toolhead["position"];
      JsonObject virtualSdcard = status["virtual_sdcard"];
      JsonObject printStats = status["print_stats"];
      JsonObject purifier = status["purifier"];

      JsonVariant cavityValue = cavity["temperature"];
      JsonVariant nozzleValue = extruder["temperature"];
      JsonVariant bedValue = bed["temperature"];
      JsonVariant zValue = position[2];
      JsonVariant progressValue = virtualSdcard["progress"];
      JsonVariant durationValue = printStats["print_duration"];
      JsonVariant speedValue = purifier["speed"];
      JsonVariant enabledValue = purifier["is_enabled"];
      const char* stateValue = printStats["state"];
      const char* extruderName = toolhead["extruder"];

      if (status && cavity && extruder && bed && toolhead && position &&
          virtualSdcard && printStats && purifier &&
          isFiniteNumber(cavityValue) && isFiniteNumber(nozzleValue) &&
          isFiniteNumber(bedValue) && isFiniteNumber(zValue) &&
          isFiniteNumber(progressValue) && isFiniteNumber(durationValue) &&
          isFiniteNumber(speedValue) && enabledValue.is<bool>() &&
          stateValue != nullptr && extruderName != nullptr) {
        float parsedCavity = cavityValue.as<float>();
        float parsedNozzle = nozzleValue.as<float>();
        float parsedBed = bedValue.as<float>();
        float parsedZ = zValue.as<float>();
        float parsedProgress = progressValue.as<float>();
        float parsedDuration = durationValue.as<float>();
        float parsedSpeed = speedValue.as<float>();

        // Reject values outside the physical/API ranges before refreshing the watchdog.
        if (parsedCavity >= 1.0f && parsedCavity <= 85.0f &&
            parsedNozzle >= 0.0f && parsedNozzle <= 400.0f &&
            parsedBed >= 0.0f && parsedBed <= 150.0f &&
            parsedZ >= -10.0f && parsedZ <= 500.0f &&
            parsedProgress >= 0.0f && parsedProgress <= 1.0f &&
            parsedDuration >= 0.0f && parsedDuration <= 4294967295.0f &&
            parsedSpeed >= 0.0f && parsedSpeed <= 100.0f) {
          chamberTemp       = parsedCavity;
          nozzleTemp        = parsedNozzle;
          bedTemp           = parsedBed;
          toolheadZ         = parsedZ;

          // Toolhead identification
          String extStr = extruderName;
          if (extStr == "extruder1")      activeTool = "T1";
          else if (extStr == "extruder2") activeTool = "T2";
          else if (extStr == "extruder3") activeTool = "T3";
          else                            activeTool = "T0";

          printProgress     = parsedProgress;
          printState        = stateValue;
          printDuration     = (unsigned long)parsedDuration;

          // Normalize purifier speed to 0.0-1.0 while accepting either API scale.
          u1PurifierSpeed = parsedSpeed > 1.0f ? parsedSpeed / 100.0f : parsedSpeed;
          u1PurifierEnabled = enabledValue.as<bool>();
          u1IsPurging = u1PurifierEnabled || u1PurifierSpeed > 0.05f;

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

  // 1. AUTONOMOUS PURGE SENSING & THERMAL SHOCK COOLDOWN GATE:
  // Manual knob PURGE triggers immediate evacuation outdoors
  if (currentMode == MODE_PURGE) {
    applyActuators(90, 255, false, false);
    return;
  }

  // Autonomous purge triggered by printer end-of-print sequence:
  if (u1IsPurging) {
    // Bed must cool to <= 60C before opening exhaust to prevent warping/thermal shock!
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
}