#include <Arduino.h>

// U1 signals
constexpr uint8_t RECIRC_INPUT = 27;  // U1 Pin 1, active-low
constexpr uint8_t EXHAUST_INPUT = 26; // U1 Pin 5, active-high via divider

// U1 PWM is approximately 997 Hz
constexpr float PWM_MIN_HZ = 700.0f;
constexpr float PWM_MAX_HZ = 1300.0f;

constexpr uint32_t PULSE_TIMEOUT_US = 30000;
constexpr uint8_t VALID_SAMPLES_REQUIRED = 3;
constexpr uint32_t PRINT_INTERVAL_MS = 500;

enum class SampleType {
  INVALID,
  PWM,
  STATIC
};

struct PwmSample {
  SampleType type = SampleType::INVALID;
  bool level = false;
  float rawDuty = 0.0f;
  float activeDuty = 0.0f;
  float frequencyHz = 0.0f;
};

struct PwmState {
  float activeDuty = 0.0f;
  float rawDuty = 100.0f;
  float frequencyHz = 0.0f;

  bool staticLevel = true;
  bool staticHigh = true;
  bool valid = false;

  uint8_t validPwmSamples = 0;
  uint8_t stableStaticSamples = 0;
  bool candidateStaticLevel = false;

  PwmSample latest;
};

PwmSample readPwm(uint8_t pin, bool activeHigh) {
  PwmSample sample;

  unsigned long highUs = pulseIn(pin, HIGH, PULSE_TIMEOUT_US);
  unsigned long lowUs = pulseIn(pin, LOW, PULSE_TIMEOUT_US);

  // A valid PWM waveform was captured.
  if (highUs > 0 && lowUs > 0) {
    const float periodUs =
        static_cast<float>(highUs) + static_cast<float>(lowUs);

    const float frequencyHz = 1000000.0f / periodUs;

    // Reject transition artifacts and impossible readings.
    if (frequencyHz < PWM_MIN_HZ || frequencyHz > PWM_MAX_HZ) {
      sample.type = SampleType::INVALID;
      return sample;
    }

    const float rawDuty =
        100.0f * static_cast<float>(highUs) / periodUs;

    sample.type = SampleType::PWM;
    sample.rawDuty = rawDuty;
    sample.activeDuty = activeHigh ? rawDuty : 100.0f - rawDuty;
    sample.frequencyHz = frequencyHz;
    return sample;
  }

  // No complete PWM cycle was captured. This may be a static level,
  // or it may be a transition. It must be confirmed by repeated samples.
  sample.type = SampleType::STATIC;
  sample.level = digitalRead(pin) == HIGH;
  sample.rawDuty = sample.level ? 100.0f : 0.0f;
  sample.activeDuty = activeHigh
                          ? sample.rawDuty
                          : 100.0f - sample.rawDuty;
  sample.frequencyHz = 0.0f;

  return sample;
}

void updatePwmState(PwmState &state, const PwmSample &sample,
                   bool activeHigh) {
  state.latest = sample;

  if (sample.type == SampleType::PWM) {
    state.validPwmSamples++;
    state.stableStaticSamples = 0;

    if (state.validPwmSamples >= VALID_SAMPLES_REQUIRED) {
      state.valid = true;
      state.staticLevel = false;
      state.rawDuty = sample.rawDuty;
      state.activeDuty = sample.activeDuty;
      state.frequencyHz = sample.frequencyHz;
    }

    return;
  }

  if (sample.type == SampleType::STATIC) {
    state.validPwmSamples = 0;

    if (state.stableStaticSamples == 0 ||
        state.candidateStaticLevel != sample.level) {
      state.candidateStaticLevel = sample.level;
      state.stableStaticSamples = 1;
      return;
    }

    state.stableStaticSamples++;

    if (state.stableStaticSamples >= VALID_SAMPLES_REQUIRED) {
      state.valid = true;
      state.staticLevel = true;
      state.staticHigh = sample.level;
      state.rawDuty = sample.rawDuty;
      state.activeDuty = sample.activeDuty;
      state.frequencyHz = 0.0f;
    }

    return;
  }

  // Invalid measurement: retain the last confirmed state.
  state.validPwmSamples = 0;
}

const char *stateName(const PwmState &state) {
  if (!state.valid) {
    return "STARTING";
  }

  if (state.staticLevel) {
    return state.activeDuty > 50.0f ? "ON / FULL" : "OFF";
  }

  return "REQUESTED / PWM";
}

void printSignal(const char *name, const PwmState &state) {
  const PwmSample &latest = state.latest;

  Serial.printf(
      "%s: active=%3.0f%% raw=%3.0f%% freq=%8.1f Hz "
      "state=%s",
      name,
      state.activeDuty,
      state.rawDuty,
      state.frequencyHz,
      stateName(state));

  if (latest.type == SampleType::INVALID) {
    Serial.print(" [ignored transition/glitch]");
  } else if (!state.valid) {
    Serial.print(" [waiting for stable reading]");
  }

  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // External resistors/divider provide the appropriate signal levels.
  pinMode(RECIRC_INPUT, INPUT);
  pinMode(EXHAUST_INPUT, INPUT);

  Serial.println();
  Serial.println("Snapmaker U1 PWM monitor");
  Serial.println("GPIO 27: U1 Pin 1, active-low recirculation");
  Serial.println("GPIO 26: U1 Pin 5, active-high exhaust via divider");
  Serial.println("No tach output is connected or generated.");
  Serial.println("Invalid PWM frequencies are rejected.");
  Serial.println();
}

void loop() {
  static PwmState recirc;
  static PwmState exhaust;

  static uint32_t lastPrintMs = 0;

  // Pin 1 is active-low.
  PwmSample recircSample = readPwm(RECIRC_INPUT, false);
  updatePwmState(recirc, recircSample, false);

  // Pin 5 is active-high.
  PwmSample exhaustSample = readPwm(EXHAUST_INPUT, true);
  updatePwmState(exhaust, exhaustSample, true);

  if (millis() - lastPrintMs >= PRINT_INTERVAL_MS) {
    lastPrintMs = millis();

    Serial.println("---");
    printSignal("RECIRC Pin 1", recirc);
    printSignal("EXHAUST Pin 5", exhaust);

    Serial.printf(
        "INFERRED: 7530=%s, Noctua=%s\n",
        stateName(recirc),
        stateName(exhaust));
  }

  delay(20);
}