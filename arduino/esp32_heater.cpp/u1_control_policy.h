#ifndef U1_CONTROL_POLICY_H
#define U1_CONTROL_POLICY_H

#include <stdint.h>

namespace u1heater {

constexpr uint32_t PREHEAT_LIMIT_MS = 20UL * 60UL * 1000UL;
constexpr uint32_t BLOWER_COOLDOWN_MS = 30UL * 1000UL;
constexpr float IDLE_BED_PREHEAT_MIN_BED_TARGET_C = 95.0f;
constexpr float IDLE_BED_PREHEAT_TARGET_C = 50.0f;
constexpr float HYSTERESIS_C = 2.0f;
constexpr float EXHAUST_HEAT_INHIBIT_PERCENT = 20.0f;

// Stable reason codes are also printed to the serial debug log.
enum class InhibitReason {
  NONE,
  INVALID_TELEMETRY,
  TELEMETRY_TIMEOUT,
  U1_POWER_UNDETECTED,
  MODE_INELIGIBLE,
  INVALID_TARGET,
  INVALID_CHAMBER_TEMPERATURE,
  BLOWER_REQUEST_OFF,
  POST_PRINT_VENTING,
  PREHEAT_TIMEOUT,
  EXHAUST_REQUEST_HIGH,
  EXHAUST_SIGNAL_UNKNOWN
};

struct Telemetry {
  bool valid;
  bool powerDetected;
  int mode;
  float targetC;
  float chamberC;
  bool heaterBedTargetValid;
  float heaterBedTargetC;
  bool printStandby;
  bool sdFileActivityValid;
  bool sdFileActive;
  // Fan speeds are normalized to 0-100 before reaching the policy.
  float innerFanPercent;
  float exhaustFanPercent;
  bool postPrintVenting;
};

struct PolicyState {
  bool heaterOn;
  bool preheatTimerStarted;
  // Expiry remains latched until telemetry leaves mode 2.
  bool preheatExpired;
  uint32_t preheatStartedAt;
  bool lastValidModeKnown;
  int lastValidMode;
  bool blowerRequested;
  bool blowerCooldownActive;
  uint32_t blowerCooldownStartedAt;
  bool idleBedPreheatTimerStarted;
  bool idleBedPreheatExpired;
  uint32_t idleBedPreheatStartedAt;
  bool idleBedPreheatActive;

  PolicyState()
      : heaterOn(false),
        preheatTimerStarted(false),
        preheatExpired(false),
        preheatStartedAt(0),
        lastValidModeKnown(false),
        lastValidMode(0),
        blowerRequested(false),
        blowerCooldownActive(false),
        blowerCooldownStartedAt(0),
        idleBedPreheatTimerStarted(false),
        idleBedPreheatExpired(false),
        idleBedPreheatStartedAt(0),
        idleBedPreheatActive(false) {}

  void cancelBlowerCooldown() {
    blowerCooldownActive = false;
    blowerCooldownStartedAt = 0;
  }

  void startBlowerCooldown(uint32_t now) {
    blowerCooldownActive = true;
    blowerCooldownStartedAt = now;
  }
};

struct Decision {
  bool blowerOn;
  bool heaterOn;
  InhibitReason reason;
};

enum class ValveFallbackAction : uint8_t { HOLD_LAST, OPEN };

inline bool inRange(float value, float minimum, float maximum) {
  // The self-comparison rejects NaN, for which all ordered comparisons fail.
  return value == value && value >= minimum && value <= maximum;
}

inline bool isStaticInputConfirmed(bool valid, uint8_t sampleCount,
                                   uint8_t requiredSamples,
                                   bool candidateLevel,
                                   bool confirmedLevel) {
  return valid && sampleCount >= requiredSamples &&
         candidateLevel == confirmedLevel;
}

inline bool enforcePreheatTimeout(PolicyState &state, uint32_t now) {
  // Unsigned subtraction keeps elapsed-time checks safe across millis() wrap.
  if (state.preheatTimerStarted &&
      static_cast<uint32_t>(now - state.preheatStartedAt) >=
          PREHEAT_LIMIT_MS) {
    state.preheatExpired = true;
    state.heaterOn = false;
  }
  return state.preheatExpired;
}

inline bool isIdleBedPreheatRequested(const Telemetry &telemetry) {
  return telemetry.valid && telemetry.powerDetected && telemetry.mode == 0 &&
         telemetry.heaterBedTargetValid &&
         inRange(telemetry.heaterBedTargetC, 0.0f, 150.0f) &&
         telemetry.heaterBedTargetC >= IDLE_BED_PREHEAT_MIN_BED_TARGET_C &&
         telemetry.printStandby && telemetry.sdFileActivityValid &&
         !telemetry.sdFileActive;
}

inline bool enforceIdleBedPreheatTimeout(PolicyState &state, uint32_t now) {
  if (state.idleBedPreheatTimerStarted && !state.idleBedPreheatExpired &&
      static_cast<uint32_t>(now - state.idleBedPreheatStartedAt) >=
          PREHEAT_LIMIT_MS) {
    state.idleBedPreheatExpired = true;
    return true;
  }
  return false;
}

inline bool expireBlowerCooldown(PolicyState &state, uint32_t now) {
  if (state.blowerCooldownActive &&
      static_cast<uint32_t>(now - state.blowerCooldownStartedAt) >=
          BLOWER_COOLDOWN_MS) {
    state.cancelBlowerCooldown();
    return true;
  }
  return false;
}

inline Decision handleTelemetryTimeout(PolicyState &state, uint32_t now) {
  const bool heaterWasOn = state.heaterOn;
  state.heaterOn = false;
  state.blowerRequested = false;
  state.lastValidModeKnown = false;
  state.idleBedPreheatActive = false;
  if (heaterWasOn) {
    state.startBlowerCooldown(now);
  }
  expireBlowerCooldown(state, now);
  return {state.blowerCooldownActive, false, InhibitReason::TELEMETRY_TIMEOUT};
}

inline ValveFallbackAction valveFallbackForUnavailableTelemetry(
    bool pwmSignalKnown, float physicalExhaustPercent) {
  if (!pwmSignalKnown ||
      !inRange(physicalExhaustPercent, 0.0f, 100.0f) ||
      physicalExhaustPercent > 0.0f) {
    return ValveFallbackAction::OPEN;
  }
  return ValveFallbackAction::HOLD_LAST;
}

inline bool isHeaterEligibleMode(int mode) {
  return mode == 2 || mode == 3;
}

inline Decision decide(const Telemetry &telemetry, PolicyState &state,
                       bool exhaustSignalValid, float physicalExhaustPercent,
                       uint32_t now) {
  // Invalid telemetry cannot safely support either actuator command.
  if (!telemetry.valid) {
    state.heaterOn = false;
    state.blowerRequested = false;
    state.lastValidModeKnown = false;
    state.idleBedPreheatActive = false;
    state.cancelBlowerCooldown();
    return {false, false, InhibitReason::INVALID_TELEMETRY};
  }

  const bool heaterWasOn = state.heaterOn;
  const bool heaterMode = isHeaterEligibleMode(telemetry.mode);
  const bool bedPreheatRequested = isIdleBedPreheatRequested(telemetry);
  const bool previousBedPreheatActive = state.idleBedPreheatActive;

  if (!bedPreheatRequested) {
    state.idleBedPreheatTimerStarted = false;
    state.idleBedPreheatExpired = false;
    state.idleBedPreheatStartedAt = 0;
  } else if (!state.idleBedPreheatTimerStarted &&
             !state.idleBedPreheatExpired) {
    state.idleBedPreheatTimerStarted = true;
    state.idleBedPreheatStartedAt = now;
  }
  const bool bedPreheatTimedOut =
      enforceIdleBedPreheatTimeout(state, now);
  const bool bedPreheatActive =
      bedPreheatRequested && !state.idleBedPreheatExpired;

  if (state.lastValidModeKnown &&
      isHeaterEligibleMode(state.lastValidMode) && !heaterMode &&
      heaterWasOn && !bedPreheatActive) {
    state.startBlowerCooldown(now);
    state.heaterOn = false;
  }
  state.lastValidMode = telemetry.mode;
  state.lastValidModeKnown = true;
  expireBlowerCooldown(state, now);

  if (telemetry.mode != 2) {
    state.preheatTimerStarted = false;
    state.preheatExpired = false;
  } else if (!state.preheatTimerStarted) {
    state.preheatTimerStarted = true;
    state.preheatStartedAt = now;
  }

  state.blowerRequested =
      inRange(telemetry.innerFanPercent, 0.0f, 100.0f) &&
      telemetry.innerFanPercent > 0.0f;
  const bool u1BlowerRequested = state.blowerRequested;
  state.idleBedPreheatActive = bedPreheatActive;
  state.blowerRequested = u1BlowerRequested || bedPreheatActive;

  if (previousBedPreheatActive && !bedPreheatActive && heaterWasOn &&
      !u1BlowerRequested) {
    state.startBlowerCooldown(now);
  }
  if (bedPreheatTimedOut && heaterWasOn && !u1BlowerRequested) {
    state.startBlowerCooldown(now);
  }
  const bool blowerOn = state.blowerRequested || state.blowerCooldownActive;

  // Mode 2 is time-limited preheat; mode 3 may maintain the requested target.
  if (telemetry.mode == 2 && enforcePreheatTimeout(state, now)) {
    state.heaterOn = false;
    return {blowerOn, false, InhibitReason::PREHEAT_TIMEOUT};
  }
  if (bedPreheatTimedOut) {
    state.heaterOn = false;
    return {blowerOn, false, InhibitReason::PREHEAT_TIMEOUT};
  }
  if (!telemetry.powerDetected) {
    state.heaterOn = false;
    return {blowerOn, false, InhibitReason::U1_POWER_UNDETECTED};
  }
  if (!heaterMode && !bedPreheatActive) {
    state.heaterOn = false;
    return {blowerOn, false, InhibitReason::MODE_INELIGIBLE};
  }
  const float heaterTargetC =
      bedPreheatActive ? IDLE_BED_PREHEAT_TARGET_C : telemetry.targetC;
  if (!inRange(heaterTargetC, 0.0f, 100.0f) || heaterTargetC <= 0.0f) {
    state.heaterOn = false;
    return {blowerOn, false, InhibitReason::INVALID_TARGET};
  }
  if (!inRange(telemetry.chamberC, -20.0f, 150.0f)) {
    state.heaterOn = false;
    return {blowerOn, false, InhibitReason::INVALID_CHAMBER_TEMPERATURE};
  }
  if (!state.blowerRequested) {
    state.heaterOn = false;
    return {blowerOn, false, InhibitReason::BLOWER_REQUEST_OFF};
  }
  if (telemetry.postPrintVenting) {
    state.heaterOn = false;
    return {blowerOn, false, InhibitReason::POST_PRINT_VENTING};
  }
  // Both the API request and measured GPIO 26 duty independently inhibit heat.
  if (!inRange(telemetry.exhaustFanPercent, 0.0f, 100.0f) ||
      telemetry.exhaustFanPercent >= EXHAUST_HEAT_INHIBIT_PERCENT) {
    state.heaterOn = false;
    return {blowerOn, false, InhibitReason::EXHAUST_REQUEST_HIGH};
  }
  if (!exhaustSignalValid) {
    state.heaterOn = false;
    return {blowerOn, false, InhibitReason::EXHAUST_SIGNAL_UNKNOWN};
  }
  if (!inRange(physicalExhaustPercent, 0.0f, 100.0f) ||
      physicalExhaustPercent >= EXHAUST_HEAT_INHIBIT_PERCENT) {
    state.heaterOn = false;
    return {blowerOn, false, InhibitReason::EXHAUST_REQUEST_HIGH};
  }

  // Preserve the previous state inside the 2 C hysteresis band.
  if (telemetry.chamberC <= heaterTargetC - HYSTERESIS_C) {
    state.heaterOn = true;
  } else if (telemetry.chamberC >= heaterTargetC) {
    state.heaterOn = false;
  }

  return {blowerOn, state.heaterOn, InhibitReason::NONE};
}

}  // namespace u1heater

#endif
