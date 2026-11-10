// Snapmaker U1 purifier PWM input test
//
// Wiring:
//   U1 Pin 2 (GND) -> ESP32 GND
//   U1 Pin 1 (recirculation, 0..3.3 V) -> 10k series resistor -> GPIO 27
//   U1 Pin 5 (exhaust, ~0..5 V) -> voltage divider -> GPIO 26
//
// Pin 5 divider:
//   Pin 5 -> 15k -> GPIO 26 -> 22k -> GND
//   5.0 V becomes approximately 2.97 V at GPIO 26.
//
// Do not connect Pin 5 directly to an ESP32 GPIO.
// Do not connect U1 Pin 6 (+24 V).

#include <Arduino.h>

constexpr uint8_t RECIRC_INPUT = 27;  // U1 Pin 1, active-low PWM
constexpr uint8_t EXHAUST_INPUT = 26; // U1 Pin 5, active-high PWM via divider
constexpr uint32_t PWM_FREQUENCY_HZ = 997;
constexpr uint32_t PULSE_TIMEOUT_US = 30000;

struct PwmReading {
  bool valid;
  bool staticLevel;
  bool staticHigh;
  float rawDuty;
  float activeDuty;
  float frequencyHz;
};

PwmReading readPwm(uint8_t pin, bool activeHigh) {
  unsigned long highUs = pulseIn(pin, HIGH, PULSE_TIMEOUT_US);
  unsigned long lowUs = pulseIn(pin, LOW, PULSE_TIMEOUT_US);

  // At 0% or 100%, there is no transition for pulseIn() to measure.
  if (highUs == 0 || lowUs == 0) {
    bool level = digitalRead(pin) == HIGH;
    PwmReading r{};
    r.valid = true;
    r.staticLevel = true;
    r.staticHigh = level;
    r.rawDuty = level ? 100.0f : 0.0f;
    r.activeDuty = activeHigh ? r.rawDuty : 100.0f - r.rawDuty;
    r.frequencyHz = 0.0f;
    return r;
  }

  float periodUs = static_cast<float>(highUs + lowUs);
  float rawDuty = 100.0f * static_cast<float>(highUs) / periodUs;

  PwmReading r{};
  r.valid = true;
  r.staticLevel = false;
  r.staticHigh = false;
  r.rawDuty = rawDuty;
  r.activeDuty = activeHigh ? rawDuty : 100.0f - rawDuty;
  r.frequencyHz = 1000000.0f / periodUs;
  return r;
}

const char* inferredState(const PwmReading& r) {
  if (!r.valid) return "NO SIGNAL";
  if (r.activeDuty < 2.0f) return "OFF";
  if (r.activeDuty > 98.0f) return "ON / FULL";
  return "REQUESTED / PWM";
}

void printReading(const char* name, const PwmReading& r) {
  Serial.printf(
      "%s: active=%6.1f%% raw=%6.1f%% freq=%7.1f Hz state=%s\n",
      name, r.activeDuty, r.rawDuty, r.frequencyHz, inferredState(r));
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(RECIRC_INPUT, INPUT);
  pinMode(EXHAUST_INPUT, INPUT);
  Serial.println();
  Serial.println("U1 dual PWM input test");
  Serial.println("Recirc Pin 1: active-low, GPIO 27");
  Serial.println("Exhaust Pin 5: active-high via divider, GPIO 26");
}

void loop() {
  PwmReading recirc = readPwm(RECIRC_INPUT, false); // PA9 is !PA9
  PwmReading exhaust = readPwm(EXHAUST_INPUT, true);

  Serial.println("---");
  printReading("RECIRC Pin 1", recirc);
  printReading("EXHAUST Pin 5", exhaust);
  Serial.printf("INFERRED: 7530=%s, Noctua=%s\n",
                inferredState(recirc), inferredState(exhaust));
  delay(500);
}
