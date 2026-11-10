// U1 fan tachulation bench test
//
// Purpose:
//   Read the U1 recirculation request on Pin 1, generate a simulated fan
//   tachometer signal, and measure that signal as though it came from U1 Pin 3.
//
// Assumptions:
//   - Nominal fan speed: 3000 RPM at 100% request.
//   - Fan tach output: 2 pulses per revolution (common 4-wire fan behavior).
//   - Therefore 3000 RPM = 100 tach pulses/second = 100 Hz.
//
// Wiring:
//   U1 Pin 1 -> 10k series resistor -> GPIO 27 (active-low PWM request)
//   U1 Pin 2 -> ESP32 GND
//   GPIO 32 (TACH_SIM_OUTPUT) -> GPIO 33 (TACH_INPUT) jumper
//
// GPIO 32 is a push-pull simulated tach source for this bench test.
// Do not connect GPIO 32 directly to a real fan tach wire while the fan is
// powered. A real fan tach is commonly open-collector and needs a pull-up.

#include <Arduino.h>

constexpr uint8_t RECIRC_INPUT = 27;
constexpr uint8_t TACH_SIM_OUTPUT = 32;
constexpr uint8_t TACH_INPUT = 33;

constexpr uint32_t REQUEST_FREQUENCY_HZ = 997;
constexpr uint32_t PULSE_TIMEOUT_US = 30000;
constexpr uint16_t LEDC_RESOLUTION_BITS = 8;
constexpr uint16_t LEDC_DUTY_HALF = 128;
constexpr float MAX_RPM = 3000.0f;
constexpr uint8_t TACH_PULSES_PER_REV = 2;

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

void setSimulatedTach(float requestedPercent) {
  requestedPercent = constrain(requestedPercent, 0.0f, 100.0f);
  float rpm = MAX_RPM * requestedPercent / 100.0f;

  if (rpm < 1.0f) {
    ledcWrite(TACH_SIM_OUTPUT, 0);
    return;
  }

  float tachHz = rpm * TACH_PULSES_PER_REV / 60.0f;
  uint32_t actualHz = static_cast<uint32_t>(roundf(tachHz));
  actualHz = max(actualHz, 1UL);
  ledcChangeFrequency(TACH_SIM_OUTPUT, actualHz, LEDC_RESOLUTION_BITS);
  ledcWrite(TACH_SIM_OUTPUT, LEDC_DUTY_HALF);
}

struct TachReading {
  bool valid;
  float frequencyHz;
  float rpm;
};

TachReading readTach() {
  unsigned long highUs = pulseIn(TACH_INPUT, HIGH, PULSE_TIMEOUT_US);
  unsigned long lowUs = pulseIn(TACH_INPUT, LOW, PULSE_TIMEOUT_US);

  TachReading r{};
  if (highUs == 0 || lowUs == 0) return r;

  float periodUs = static_cast<float>(highUs + lowUs);
  r.valid = true;
  r.frequencyHz = 1000000.0f / periodUs;
  r.rpm = r.frequencyHz * 60.0f / TACH_PULSES_PER_REV;
  return r;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RECIRC_INPUT, INPUT);
  pinMode(TACH_INPUT, INPUT);
  pinMode(TACH_SIM_OUTPUT, OUTPUT);
  digitalWrite(TACH_SIM_OUTPUT, LOW);

  if (!ledcAttach(TACH_SIM_OUTPUT, 100, LEDC_RESOLUTION_BITS)) {
    Serial.println("ERROR: LEDC tach output attach failed");
  }
  ledcWrite(TACH_SIM_OUTPUT, 0);

  Serial.println();
  Serial.println("U1 fan tach simulation test");
  Serial.println("Pin 1: GPIO 27, active-low request");
  Serial.println("Tach simulator: GPIO 32 -> GPIO 33 jumper");
  Serial.println("Model: 3000 RPM max, 2 pulses/rev");
  Serial.println("Expected: 100 Hz and approximately 3000 RPM at FULL");
}

void loop() {
  PwmReading request = readPwm(RECIRC_INPUT, false); // U1 PA9 is active-low
  setSimulatedTach(request.activeDuty);
  delay(2); // allow the LEDC frequency change to settle

  TachReading tach = readTach();
  float expectedRpm = MAX_RPM * request.activeDuty / 100.0f;

  Serial.printf(
      "REQUEST: %6.1f%% | EXPECTED: %7.1f RPM (%6.1f Hz) | "
      "MEASURED: %7.1f RPM (%6.1f Hz) | %s\n",
      request.activeDuty,
      expectedRpm,
      expectedRpm * TACH_PULSES_PER_REV / 60.0f,
      tach.valid ? tach.rpm : 0.0f,
      tach.valid ? tach.frequencyHz : 0.0f,
      tach.valid ? "VALID" : "NO TACH");

  delay(500);
}
