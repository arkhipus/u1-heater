#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
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

const unsigned long DISPLAY_INTERVAL_MS = 250UL; // 4 FPS update rate

// --- State Enums ---
enum Mode { MODE_OFF, MODE_LOW, MODE_MED, MODE_HI, MODE_PURGE };

// --- Hardware Objects ---
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Servo flapServo;

// --- Actuator State Tracking ---
int currentServoAngle = 0;
int currentNoctuaPWM  = 0;
bool currentBlowerOn  = false;
bool currentHeaterOn  = false;
bool autoPurgeActive  = false;

// --- State & Telemetry Variables ---
Mode currentMode = MODE_OFF;
float chamberTemp = 0.0;
float nozzleTemp  = 0.0;
float bedTemp     = 0.0;
float toolheadZ   = 0.0;
String activeTool = "T0";
float printProgress = 0.0;
unsigned long printDuration = 0;
String printState = "offline";
float u1PurifierSpeed = 0.0;

// --- Timers (Restoring the missing ones!) ---
unsigned long lastPollTime           = 0;
unsigned long lastValidPollTime      = 0;
unsigned long lastDisplayTime        = 0;
unsigned long lastReconnectAttempt   = 0;
unsigned long autoPurgeStartTime     = 0;

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
  Wire.setClock(400000);
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    
    // Top Yellow Zone (Y=0..15): clean at Y=4 (occupies Y=4..11)
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

  // Poll Moonraker API every 2 seconds
  if (now - lastPollTime >= POLL_INTERVAL_MS) {
    lastPollTime = now;
    pollMoonraker();
  }

  // Check 1-minute safety timeout on telemetry
  bool commsTimeout = (lastValidPollTime > 0 && (millis() - lastValidPollTime >= COMM_TIMEOUT_MS));

  if (commsTimeout) {
    // 1. Kill the heater for safety while telemetry is missing
    digitalWrite(PIN_HEATER_RELAY, HIGH); // Active-LOW: HIGH = OFF
    
    // 2. Lock the screen on the Warning display (Do NOT draw dashboard!)
    displayTimeoutWarning(now);
  } 
  else {
    // Telemetry is healthy: Execute normal heating logic
    executeChamberLogic(now);

    // ONLY draw the normal dashboard when healthy!
    if (now - lastDisplayTime >= DISPLAY_INTERVAL_MS) {
      lastDisplayTime = now;
      updateDisplay();
    }
  }
}

void handleWifiDisconnected(unsigned long now) {
  // Safe elapsed calculation (no unsigned underflow!)
  unsigned long elapsedWithoutData = (lastValidPollTime > 0) ? (now - lastValidPollTime) : now;
  bool timeoutExceeded = (elapsedWithoutData >= COMM_TIMEOUT_MS);

  if (timeoutExceeded) {
    // SAFETY CUTOFF: Temporarily suspend heater while waiting for connection
    digitalWrite(PIN_HEATER_RELAY, HIGH); // Active-LOW: HIGH = OFF
  }

  // Display "NO WI-FI" screen with status
  display.clearDisplay();
  
  // Yellow Header (Rows 0-15)
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("NETWORK: [CONNECTING]");
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
    display.print("Attempting rejoin...");
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

  delay(500); 
}

void displayTimeoutWarning(unsigned long now) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("TELEMETRY: [WAITING]");
  display.drawLine(0, 12, 128, 12, SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 20);
  display.print("NO TELEMETRY > 1 MIN");
  display.setCursor(0, 34);
  display.print("HEATER: [SUSPENDED]");
  display.setCursor(0, 48);
  display.print("AUTO-RESUMES ON DATA");
  display.display();
}

// Mode readSwitchMode() {
//   int rawADC = analogRead(PIN_SWITCH_ADC); // 0 to 4095
//   // Voltage bands for 5 positions (approx 0V, 0.82V, 1.65V, 2.48V, 3.3V)
//   if (rawADC < 400)  return MODE_OFF;    // ~0.0V
//   if (rawADC < 1500) return MODE_LOW;    // ~0.82V
//   if (rawADC < 2500) return MODE_MED;    // ~1.65V
//   if (rawADC < 3500) return MODE_HI;     // ~2.48V
//   return MODE_PURGE;                     // ~3.3V
// }

Mode readSwitchMode() {
  // Take 10 readings and average them to eliminate ADC noise
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(PIN_SWITCH_ADC);
  }
  int rawADC = sum / 10;

  // Voltage bands with clean thresholds
  if (rawADC < 400)  return MODE_OFF;    // ~0.0V
  if (rawADC < 1500) return MODE_LOW;    // ~0.82V
  if (rawADC < 2500) return MODE_MED;    // ~1.65V
  if (rawADC < 3500) return MODE_HI;     // ~2.48V
  return MODE_PURGE;                     // ~3.3V
}

// void pollMoonraker() {
//   HTTPClient http;
//   String url = "http://" + String(U1_IP_ADDRESS) + 
//                "/printer/objects/query?print_stats&virtual_sdcard&temperature_sensor%20cavity&purifier";
//   http.begin(url);
//   http.setTimeout(1500);

//   int httpCode = http.GET();

//   Serial.print("Moonraker HTTP Code: ");
//   Serial.println(httpCode);

//   if (httpCode == 200) {
//     String payload = http.getString();
//     // StaticJsonDocument<768> doc;
//     // NEW (ArduinoJson v7):
//     JsonDocument doc;
//     DeserializationError error = deserializeJson(doc, payload);

//     if (error) {
//       Serial.print("JSON Error: ");
//       Serial.println(error.c_str());
//     } else {
//       Serial.println("JSON Parsed OK!");
//     }

//     if (!error) {
//       JsonObject status = doc["result"]["status"];
//       chamberTemp       = status["temperature_sensor cavity"]["temperature"] | 0.0;
//       printProgress     = status["virtual_sdcard"]["progress"] | 0.0;
//       printState        = status["print_stats"]["state"].as<String>();
//       u1PurifierSpeed   = status["purifier"]["speed"] | 0.0;
      
//       // AUTO-RESUME: Fresh valid packet received! 
//       lastValidPollTime = millis(); 
//     }
//   }
//   http.end();
// }

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
      chamberTemp   = status["temperature_sensor cavity"]["temperature"] | 0.0;
      nozzleTemp    = status["extruder"]["temperature"] | 0.0;
      bedTemp       = status["heater_bed"]["temperature"] | 0.0;
      toolheadZ     = status["toolhead"]["position"][2] | 0.0;
      
      // Map active extruder string to T0, T1, T2, T3
      String extStr = status["toolhead"]["extruder"] | "extruder";
      if (extStr == "extruder1")      activeTool = "T1";
      else if (extStr == "extruder2") activeTool = "T2";
      else if (extStr == "extruder3") activeTool = "T3";
      else                            activeTool = "T0";

      printProgress     = status["virtual_sdcard"]["progress"] | 0.0;
      printState        = status["print_stats"]["state"].as<String>();
      printDuration     = status["print_stats"]["print_duration"] | 0UL;
      u1PurifierSpeed   = status["purifier"]["speed"] | 0.0;
      
      lastValidPollTime = millis(); 
    }
  }
  http.end();
}

void executeChamberLogic(unsigned long now) {
  // 1. AUTONOMOUS PURGE SENSING:
  bool u1IsPurging = (u1PurifierSpeed > 0.05);

  if ((u1IsPurging || currentMode == MODE_PURGE) && currentMode != MODE_OFF) {
    int exhaustPWM = u1IsPurging ? map(u1PurifierSpeed * 100, 0, 100, 0, 255) : 255;
    applyActuators(90, exhaustPWM, false, false); // Valve 90, Noctua ON, Blower OFF, Heat OFF
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
      bool shouldHeat = false;
      // Regulate at 50C with 1.0C hysteresis
      if (chamberTemp > 0.0 && chamberTemp < 49.0) {
        shouldHeat = true;
      }
      applyActuators(0, 0, true, shouldHeat); // Valve 0, Noctua 0, Blower ON, Heat on demand
      break;
    }

    case MODE_PURGE:
      applyActuators(90, 255, false, false);
      break;
  }
}

// void applyActuators(int servoAngle, int noctuaPWM, bool blowerOn, bool heatEnable) {
//   flapServo.write(servoAngle);
//   analogWrite(PIN_NOCTUA_PWM, noctuaPWM);
//   digitalWrite(PIN_BLOWER, blowerOn ? HIGH : LOW);
//   // Active-LOW SSR: LOW = ON (Heater energized), HIGH = OFF (Heater dead)
//   digitalWrite(PIN_HEATER_RELAY, heatEnable ? LOW : HIGH);
// }

void applyActuators(int servoAngle, int noctuaPWM, bool blowerOn, bool heatEnable) {
  // ========================================================
  // SAFETY INTERLOCK: Never blow exhaust against closed flap!
  // ========================================================
  if (servoAngle == 0) {
    noctuaPWM = 0;   // If the valve is closed, force the exhaust fan OFF!
  } else if (noctuaPWM > 0 && servoAngle < 45) {
    servoAngle = 90; // If exhaust fan is running, force valve wide open!
  }

  // Save current commanded state
  currentServoAngle = servoAngle;
  currentNoctuaPWM  = noctuaPWM;
  currentBlowerOn   = blowerOn;
  currentHeaterOn   = heatEnable;

  // Send to hardware
  flapServo.write(servoAngle);
  analogWrite(PIN_NOCTUA_PWM, noctuaPWM);
  digitalWrite(PIN_BLOWER, blowerOn ? HIGH : LOW);
  digitalWrite(PIN_HEATER_RELAY, heatEnable ? LOW : HIGH);
}

// void updateDisplay() {
//   display.clearDisplay();

// // Mode Header (Yellow Band, Rows 0-15) - Clean & Compact!
//   display.setTextSize(1);
//   display.setCursor(0, 0);
//   switch (currentMode) {
//     case MODE_OFF:   display.print("M: OFF/STDBY");   break;
//     case MODE_LOW:   display.print("M: LOW/PLA");   break;
//     case MODE_MED:   display.print("M: MED/PETG");  break;
//     case MODE_HI:    display.print("M: HI/ASA");   break;
//     case MODE_PURGE: display.print("M: *PURGE*"); break;
//   }

//   // Right-aligned Progress % or WiFi status (No overlap!)
//   if (printState == "printing") {
//     String progStr = String(int(printProgress * 100)) + "%";
//     int xPos = 128 - (progStr.length() * 6); // Dynamically hugs the right edge
//     display.setCursor(xPos, 0);
//     display.print(progStr);
//   } else {
//     display.setCursor(114, 0);
//     display.print(WiFi.status() == WL_CONNECTED ? "WF" : "--");
//   }

//   // Decorative divider
//   display.drawLine(0, 12, 128, 12, SSD1306_WHITE);

//   // Main Data (Blue Band, Rows 16-63)
//   display.setCursor(0, 18);
//   display.setTextSize(2);
//   display.print(chamberTemp, 1);
//   display.setTextSize(1);
//   display.print(" C");

//   display.setCursor(68, 18);
//   // Active-LOW check: LOW = Heating
//   if (digitalRead(PIN_HEATER_RELAY) == LOW) {
//     display.print("HEATING");
//   } else {
//     display.print("HOLDING");
//   }

//   display.setCursor(68, 28);
//   display.print(printState);

//   // Actuator Status on Bottom Row - Rock Solid!
//   display.setCursor(0, 52);
//   display.print("VLV:");
//   display.print(currentServoAngle == 0 ? "CLS " : "OPN ");

//   display.print("EXH:");
//   display.print(map(currentNoctuaPWM, 0, 255, 0, 100));
//   display.print("%");

//   display.display();
// }

// Formats seconds into clean "30:23" (MM:SS or HH:MM) in exactly 5 characters
void printColonTime(unsigned long totalSecs) {
  unsigned long hrs  = totalSecs / 3600;
  unsigned long mins = (totalSecs % 3600) / 60;
  unsigned long secs = totalSecs % 60;

  if (hrs > 0) {
    // If >= 1 hour: format as HH:MM (e.g. 01:42)
    if (hrs < 10) display.print("0");
    display.print(hrs);
    display.print(":");
    if (mins < 10) display.print("0");
    display.print(mins);
  } else {
    // If < 1 hour: format as MM:SS (e.g. 30:23)
    if (mins < 10) display.print("0");
    display.print(mins);
    display.print(":");
    if (secs < 10) display.print("0");
    display.print(secs);
  }
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextWrap(false); // Prevents line dropping if a string touches pixel 128

  // ========================================================
  // 1. TOP YELLOW BAND (Rows 0 - 15): Mode & Graphical Progress
  // ========================================================
  display.setTextSize(1);
  display.setCursor(0, 2);
  switch (currentMode) {
    case MODE_OFF:   display.print("OFF");       break;
    case MODE_LOW:   display.print("LOW [PLA]"); break;
    case MODE_MED:   display.print("MED [PETG]");break;
    case MODE_HI:    display.print("HIGH [ASA]");break;
    case MODE_PURGE: display.print("PURGE");     break;
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

    // Line 4 (Y = 49): Elapsed Print Time (TME) & Remaining ETA in "30:23" format
  display.setCursor(0, 49);
  if (printState == "printing") {
    display.print("TME:");
    printColonTime(printDuration);

    // Starting at X=62: "| ETA:" (6 chars) + "30:23" (5 chars) = exactly 11 chars (66px)
    // 62 + 66 = 128px (exact fit on screen with zero clipping!)
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