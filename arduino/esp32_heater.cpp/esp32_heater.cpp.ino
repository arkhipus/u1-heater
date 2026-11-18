#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>
#include <string.h>

#include "bench_test_config.h"
#include "secrets.h"
#include "u1_control_policy.h"

// GPIO 26 monitors U1 Pin 5; the ESP32 never generates the exhaust PWM.
constexpr uint8_t PIN_SERVO = 18;
constexpr uint8_t PIN_BLOWER = 23;
constexpr uint8_t PIN_HEATER_RELAY = 25;
constexpr uint8_t PIN_EXHAUST_PWM_MONITOR = 26;
constexpr uint8_t PIN_RECIRC_PWM_MONITOR = 27;
constexpr uint8_t PIN_I2C_SDA = 21;
constexpr uint8_t PIN_I2C_SCL = 22;
constexpr uint8_t HEATER_RELAY_ON_LEVEL =
    HEATER_RELAY_ACTIVE_LOW ? LOW : HIGH;
constexpr uint8_t HEATER_RELAY_OFF_LEVEL =
    HEATER_RELAY_ACTIVE_LOW ? HIGH : LOW;

constexpr uint8_t SCREEN_WIDTH = 128;
constexpr uint8_t SCREEN_HEIGHT = 64;
constexpr int8_t OLED_RESET = -1;
constexpr uint8_t OLED_I2C_ADDR = 0x3C;

constexpr uint32_t POLL_INTERVAL_MS = 2000;
constexpr uint32_t HTTP_TIMEOUT_MS = 1500;
constexpr uint32_t TELEMETRY_GRACE_MS = 60000;
constexpr uint32_t RECONNECT_INTERVAL_MS = 10000;
constexpr uint32_t DISPLAY_INTERVAL_MS = 250;
constexpr uint32_t IP_SPLASH_DURATION_MS = 10000;
constexpr float CPU_TEMP_FILTER_ALPHA = 0.1f;
constexpr uint32_t VALVE_CLOSE_DELAY_MS = 500;
constexpr uint32_t PWM_SAMPLE_TIMEOUT_US = 2500;
constexpr uint32_t PWM_STALE_MS = 500;
constexpr uint8_t PWM_SAMPLES_REQUIRED = 3;
constexpr float PWM_MIN_HZ = 700.0f;
constexpr float PWM_MAX_HZ = 1300.0f;
constexpr uint8_t SERVO_CLOSED_DEG = 180;
constexpr uint8_t SERVO_OPEN_DEG = 80;

enum class SampleType : uint8_t { INVALID, PWM, STATIC };
enum class LinkState : uint8_t { WAITING, OK, LOST, EXPIRED, INVALID };

struct PwmSample {
  SampleType type = SampleType::INVALID;
  bool level = false;
  // rawDuty is electrical HIGH time; activeDuty accounts for signal polarity.
  float rawDuty = 0.0f;
  float activeDuty = 0.0f;
  float frequencyHz = 0.0f;
};

struct PwmState {
  PwmSample latest;
  float activeDuty = 0.0f;
  float rawDuty = 0.0f;
  float frequencyHz = 0.0f;
  uint32_t lastValidMs = 0;
  uint8_t validPwmSamples = 0;
  uint8_t stableStaticSamples = 0;
  bool candidateStaticLevel = false;
  bool confirmedStaticLevel = false;
  bool valid = false;
};

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WebServer statusServer(80);
Servo flapServo;
PwmState exhaustPwm;
PwmState recircPwm;
u1heater::PolicyState policyState;
u1heater::Telemetry telemetry = {};

bool displayAvailable = false;
bool telemetryEverValid = false;
bool telemetryInvalid = false;
bool telemetryTransportLost = false;
bool progressValid = false;
bool valveInactiveTiming = false;
bool currentBlowerOn = false;
bool currentHeaterOn = false;
bool valveOpen = true;
bool pollScheduled = false;
float printProgress = 0.0f;
float filteredCpuTempC = 0.0f;
bool cpuTempFilterInitialized = false;
bool ipSplashStarted = false;
uint32_t ipSplashStartedAt = 0;
String startupIpAddress;
uint32_t lastValidPollTime = 0;
uint32_t lastPollTime = 0;
uint32_t lastDisplayTime = 0;
uint32_t lastReconnectAttempt = 0;
uint32_t valveInactiveSince = 0;
uint8_t currentServoAngle = SERVO_OPEN_DEG;
LinkState linkState = LinkState::WAITING;
u1heater::InhibitReason inhibitReason =
    u1heater::InhibitReason::INVALID_TELEMETRY;
String printState = "offline";

bool isFiniteNumber(JsonVariantConst value) {
  if (!value.is<float>() && !value.is<double>() &&
      !value.is<int>() && !value.is<unsigned int>() &&
      !value.is<long>() && !value.is<unsigned long>()) {
    return false;
  }
  return isfinite(value.as<float>());
}

bool readNumber(JsonVariantConst value, float &result) {
  if (!isFiniteNumber(value)) {
    return false;
  }
  result = value.as<float>();
  return true;
}

// Moonraker fan speeds may be fractions (0-1) or percentages (0-100).
bool normalizeFanSpeed(JsonVariantConst value, float &percent) {
  float raw = 0.0f;
  if (!readNumber(value, raw) || raw < 0.0f || raw > 100.0f) {
    return false;
  }
  percent = raw <= 1.0f ? raw * 100.0f : raw;
  return true;
}

bool isTerminalPrintState(const char *state) {
  if (state == nullptr) {
    return false;
  }
  return strcmp(state, "complete") == 0 ||
         strcmp(state, "cancelled") == 0 ||
         strcmp(state, "error") == 0;
}

bool isKnownPrintState(const char *state) {
  if (state == nullptr) {
    return false;
  }
  return strcmp(state, "standby") == 0 || strcmp(state, "printing") == 0 ||
         strcmp(state, "paused") == 0 || strcmp(state, "complete") == 0 ||
         strcmp(state, "cancelled") == 0 || strcmp(state, "error") == 0;
}

// activeHigh=false normalizes the active-low recirculation monitor.
PwmSample readPwm(uint8_t pin, bool activeHigh) {
  PwmSample sample;
  const uint32_t highUs = pulseIn(pin, HIGH, PWM_SAMPLE_TIMEOUT_US);
  const uint32_t lowUs = pulseIn(pin, LOW, PWM_SAMPLE_TIMEOUT_US);

  if (highUs > 0 && lowUs > 0) {
    const float periodUs = static_cast<float>(highUs + lowUs);
    const float frequencyHz = 1000000.0f / periodUs;
    if (frequencyHz < PWM_MIN_HZ || frequencyHz > PWM_MAX_HZ) {
      return sample;
    }

    sample.type = SampleType::PWM;
    sample.rawDuty = 100.0f * static_cast<float>(highUs) / periodUs;
    sample.activeDuty = activeHigh ? sample.rawDuty : 100.0f - sample.rawDuty;
    sample.frequencyHz = frequencyHz;
    return sample;
  }

  sample.type = SampleType::STATIC;
  sample.level = digitalRead(pin) == HIGH;
  sample.rawDuty = sample.level ? 100.0f : 0.0f;
  sample.activeDuty = activeHigh ? sample.rawDuty : 100.0f - sample.rawDuty;
  return sample;
}

// Require three consecutive PWM samples or stable static samples before
// trusting a change, so transient edges do not become actuator commands.
void updatePwmState(PwmState &state, const PwmSample &sample, uint32_t now) {
  state.latest = sample;

  if (sample.type == SampleType::PWM) {
    if (state.validPwmSamples < PWM_SAMPLES_REQUIRED) {
      state.validPwmSamples++;
    }
    state.stableStaticSamples = 0;
    if (state.validPwmSamples >= PWM_SAMPLES_REQUIRED) {
      state.valid = true;
      state.activeDuty = sample.activeDuty;
      state.rawDuty = sample.rawDuty;
      state.frequencyHz = sample.frequencyHz;
      state.lastValidMs = now;
    }
    return;
  }

  state.validPwmSamples = 0;
  if (sample.type == SampleType::STATIC) {
    if (state.stableStaticSamples == 0 ||
        state.candidateStaticLevel != sample.level) {
      state.candidateStaticLevel = sample.level;
      state.stableStaticSamples = 1;
      return;
    }

    if (state.stableStaticSamples < PWM_SAMPLES_REQUIRED) {
      state.stableStaticSamples++;
    }
    if (state.stableStaticSamples >= PWM_SAMPLES_REQUIRED) {
      state.valid = true;
      state.confirmedStaticLevel = sample.level;
      state.activeDuty = sample.activeDuty;
      state.rawDuty = sample.rawDuty;
      state.frequencyHz = 0.0f;
      state.lastValidMs = now;
    }
    return;
  }

  state.stableStaticSamples = 0;
  if (now - state.lastValidMs > PWM_STALE_MS) {
    state.valid = false;
  }
}

// Keep relay polarity configurable for different heater switching modules.
void setHeater(bool enabled) {
  currentHeaterOn = enabled;
  digitalWrite(PIN_HEATER_RELAY,
               enabled ? HEATER_RELAY_ON_LEVEL : HEATER_RELAY_OFF_LEVEL);
}

void setBlower(bool enabled) {
  currentBlowerOn = enabled;
  digitalWrite(PIN_BLOWER, enabled ? HIGH : LOW);
}

void setValve(bool open) {
  if (valveOpen == open) {
    return;
  }
  valveOpen = open;
  currentServoAngle = open ? SERVO_OPEN_DEG : SERVO_CLOSED_DEG;
  flapServo.write(currentServoAngle);
  Serial.printf("VALVE command=%s angle=%u\n", open ? "OPEN" : "CLOSED",
                static_cast<unsigned int>(currentServoAngle));
}

bool pwmSignalStable(const PwmState &state) {
  if (!state.valid || state.latest.type == SampleType::INVALID) {
    return false;
  }
  if (state.latest.type == SampleType::PWM) {
    return state.validPwmSamples >= PWM_SAMPLES_REQUIRED;
  }
  return u1heater::isStaticInputConfirmed(
      state.valid, state.stableStaticSamples, PWM_SAMPLES_REQUIRED,
      state.candidateStaticLevel, state.confirmedStaticLevel);
}

// Unknown inputs fail open. Without Moonraker data, GPIO 26 is the only exhaust
// request source; a confirmed inactive request holds the current valve state.
void updateValveCommand(uint32_t now) {
  const bool signalUnknown =
      !exhaustPwm.valid || now - exhaustPwm.lastValidMs > PWM_STALE_MS;
  const float observedDuty =
      exhaustPwm.latest.type == SampleType::PWM
          ? exhaustPwm.latest.activeDuty
          : exhaustPwm.activeDuty;

  if (!telemetryEverValid) {
    valveInactiveTiming = false;
    if (!valveOpen) {
      Serial.println("VALVE OPEN cause=WAITING_FOR_FIRST_TELEMETRY");
    }
    setValve(true);
    return;
  }

  const bool telemetryUnavailable =
      !telemetry.valid || telemetryInvalid || telemetryTransportLost ||
      linkState != LinkState::OK;
  if (telemetryUnavailable) {
    valveInactiveTiming = false;
    const u1heater::ValveFallbackAction action =
        u1heater::valveFallbackForUnavailableTelemetry(!signalUnknown,
                                                       observedDuty);
    if (action == u1heater::ValveFallbackAction::OPEN) {
      if (!valveOpen) {
        Serial.printf(
            "VALVE OPEN cause=%s telemetry_unavailable=1 pwm_valid=%d "
            "pwm_age_ms=%lu duty=%.1f\n",
            signalUnknown ? "PWM_UNKNOWN" : "PWM_REQUEST", exhaustPwm.valid,
            static_cast<unsigned long>(now - exhaustPwm.lastValidMs),
            observedDuty);
      }
      setValve(true);
    }
    return;
  }

  if (signalUnknown) {
    valveInactiveTiming = false;
    if (!valveOpen) {
      Serial.printf(
          "VALVE OPEN cause=PWM_UNKNOWN telemetry_valid=%d link=%s "
          "pwm_valid=%d pwm_age_ms=%lu sample_type=%d "
          "latest_duty=%.1f confirmed_duty=%.1f\n",
          telemetry.valid, linkStateName(linkState), exhaustPwm.valid,
          static_cast<unsigned long>(now - exhaustPwm.lastValidMs),
          static_cast<int>(exhaustPwm.latest.type),
          exhaustPwm.latest.activeDuty, exhaustPwm.activeDuty);
    }
    setValve(true);
    return;
  }

  const bool telemetryRequestsExhaust =
      telemetry.exhaustFanPercent > 0.0f;
  if (telemetryRequestsExhaust || observedDuty > 0.0f) {
    valveInactiveTiming = false;
    if (!valveOpen) {
      Serial.printf(
          "VALVE OPEN cause=%s api_exhaust=%.1f gpio26_duty=%.1f\n",
          telemetryRequestsExhaust ? "API_REQUEST" : "PWM_REQUEST",
          telemetry.exhaustFanPercent, observedDuty);
    }
    setValve(true);
    return;
  }

  if (!valveInactiveTiming) {
    valveInactiveTiming = true;
    valveInactiveSince = now;
  } else if (now - valveInactiveSince >= VALVE_CLOSE_DELAY_MS) {
    setValve(false);
  }
}

const char *inhibitReasonName(u1heater::InhibitReason reason) {
  using u1heater::InhibitReason;
  switch (reason) {
    case InhibitReason::NONE: return "NONE";
    case InhibitReason::INVALID_TELEMETRY: return "INVALID_TELEMETRY";
    case InhibitReason::TELEMETRY_TIMEOUT: return "TELEMETRY_TIMEOUT";
    case InhibitReason::U1_POWER_UNDETECTED: return "U1_POWER_UNDETECTED";
    case InhibitReason::MODE_INELIGIBLE: return "MODE_INELIGIBLE";
    case InhibitReason::INVALID_TARGET: return "INVALID_TARGET";
    case InhibitReason::INVALID_CHAMBER_TEMPERATURE:
      return "INVALID_CHAMBER_TEMPERATURE";
    case InhibitReason::BLOWER_REQUEST_OFF: return "BLOWER_REQUEST_OFF";
    case InhibitReason::POST_PRINT_VENTING: return "POST_PRINT_VENTING";
    case InhibitReason::PREHEAT_TIMEOUT: return "PREHEAT_TIMEOUT";
    case InhibitReason::EXHAUST_REQUEST_HIGH: return "EXHAUST_REQUEST_HIGH";
    case InhibitReason::EXHAUST_SIGNAL_UNKNOWN:
      return "EXHAUST_SIGNAL_UNKNOWN";
  }
  return "UNKNOWN";
}

const char *linkStateName(LinkState state) {
  switch (state) {
    case LinkState::WAITING: return "WAITING";
    case LinkState::OK: return "OK";
    case LinkState::LOST: return "LOST";
    case LinkState::EXPIRED: return "EXPIRED";
    case LinkState::INVALID: return "INVALID";
  }
  return "UNKNOWN";
}

const char *purifierModeName(int mode) {
  switch (mode) {
    case 0: return "IDLE";
    case 1: return "COOLING";
    case 2: return "PREHEAT";
    case 3: return "HOT_CHAMBER";
  }
  return "UNKNOWN";
}

void handleStatusRequest() {
  JsonDocument response;
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  response["device"] = "u1-heater";
  response["wifi_connected"] = wifiConnected;
  if (wifiConnected) {
    const IPAddress ip = WiFi.localIP();
    response["ip_address"] = ip.toString();
    response["ip_last_octet"] = ip[3];
  } else {
    response["ip_address"] = nullptr;
    response["ip_last_octet"] = nullptr;
  }

  JsonObject telemetryStatus = response["telemetry"].to<JsonObject>();
  telemetryStatus["valid"] = telemetry.valid;
  telemetryStatus["fresh"] =
      telemetry.valid && linkState == LinkState::OK;
  telemetryStatus["link_state"] = linkStateName(linkState);
  if (telemetryEverValid) {
    telemetryStatus["age_ms"] =
        static_cast<uint32_t>(millis() - lastValidPollTime);
  } else {
    telemetryStatus["age_ms"] = nullptr;
  }
  telemetryStatus["print_state"] = printState;
  if (telemetry.valid) {
    telemetryStatus["purifier_mode"] = telemetry.mode;
    telemetryStatus["purifier_mode_name"] =
        purifierModeName(telemetry.mode);
    telemetryStatus["chamber_c"] = telemetry.chamberC;
    telemetryStatus["target_c"] = telemetry.targetC;
    if (telemetry.heaterBedTargetValid) {
      telemetryStatus["bed_target_c"] = telemetry.heaterBedTargetC;
    } else {
      telemetryStatus["bed_target_c"] = nullptr;
    }
    telemetryStatus["bed_preheat_active"] =
        policyState.idleBedPreheatActive;
    telemetryStatus["sd_file_activity_valid"] =
        telemetry.sdFileActivityValid;
    if (telemetry.sdFileActivityValid) {
      telemetryStatus["sd_file_active"] = telemetry.sdFileActive;
    } else {
      telemetryStatus["sd_file_active"] = nullptr;
    }
    telemetryStatus["inner_fan_request_percent"] =
        telemetry.innerFanPercent;
    telemetryStatus["exhaust_request_percent"] =
        telemetry.exhaustFanPercent;
    telemetryStatus["post_print_venting"] = telemetry.postPrintVenting;
  } else {
    telemetryStatus["purifier_mode"] = nullptr;
    telemetryStatus["purifier_mode_name"] = nullptr;
    telemetryStatus["chamber_c"] = nullptr;
    telemetryStatus["target_c"] = nullptr;
    telemetryStatus["bed_target_c"] = nullptr;
    telemetryStatus["bed_preheat_active"] = false;
    telemetryStatus["sd_file_activity_valid"] = false;
    telemetryStatus["sd_file_active"] = nullptr;
    telemetryStatus["inner_fan_request_percent"] = nullptr;
    telemetryStatus["exhaust_request_percent"] = nullptr;
    telemetryStatus["post_print_venting"] = nullptr;
  }

  JsonObject outputs = response["outputs"].to<JsonObject>();
  outputs["heater_commanded_on"] = currentHeaterOn;
  outputs["heater_inhibit_reason"] = inhibitReasonName(inhibitReason);
  outputs["blower_commanded_on"] = currentBlowerOn;
  outputs["valve_commanded_open"] = valveOpen;
  outputs["servo_angle_deg"] = currentServoAngle;
  outputs["exhaust_pwm_known"] = pwmSignalStable(exhaustPwm);
  if (pwmSignalStable(exhaustPwm)) {
    outputs["physical_exhaust_request_percent"] = exhaustPwm.activeDuty;
  } else {
    outputs["physical_exhaust_request_percent"] = nullptr;
  }

  if (cpuTempFilterInitialized && isfinite(filteredCpuTempC)) {
    response["cpu_temperature_c"] = filteredCpuTempC;
  } else {
    response["cpu_temperature_c"] = nullptr;
  }

  String body;
  serializeJson(response, body);
  statusServer.sendHeader("Cache-Control", "no-store");
  statusServer.send(200, "application/json", body);
}

void handleStatusNotFound() {
  statusServer.send(404, "application/json", "{\"error\":\"not_found\"}");
}

void debugSnapshot() {
  Serial.printf(
      "GPIO27 valid=%d duty=%.1f%% Hz=%.1f; "
      "GPIO26 valid=%d duty=%.1f%% Hz=%.1f\n",
      recircPwm.valid, recircPwm.activeDuty, recircPwm.frequencyHz,
      exhaustPwm.valid, exhaustPwm.activeDuty, exhaustPwm.frequencyHz);
  Serial.printf(
      "OUTPUT heater=%d blower=%d valve=%s wifi=%d link=%s inhibit=%s\n",
      currentHeaterOn, currentBlowerOn, valveOpen ? "OPEN" : "CLOSED",
      WiFi.status() == WL_CONNECTED, linkStateName(linkState),
      inhibitReasonName(inhibitReason));
}

void debugTelemetry(const String &payload) {
  Serial.println("MOONRAKER RAW:");
  Serial.println(payload);
  Serial.printf(
      "PARSED mode=%d power=%d target=%.2fC chamber=%.2fC "
      "bed_target=%.2fC bed_valid=%d sd_active=%d/%d "
      "inner=%.1f%% exhaust=%.1f%% print=%s\n",
      telemetry.mode, telemetry.powerDetected, telemetry.targetC,
      telemetry.chamberC, telemetry.heaterBedTargetC,
      telemetry.heaterBedTargetValid, telemetry.sdFileActive,
      telemetry.sdFileActivityValid, telemetry.innerFanPercent,
      telemetry.exhaustFanPercent, printState.c_str());
  debugSnapshot();
}

void applyTelemetry(const String &payload, JsonDocument &document) {
  JsonObject result = document["result"];
  JsonObject status = result["status"];
  JsonObject purifier = status["purifier"];
  JsonObject exhaustFan = purifier["exhaust_fan"];
  JsonObject innerFan = purifier["inner_fan"];
  JsonObject cavity = status["temperature_sensor cavity"];
  JsonObject printStats = status["print_stats"];
  JsonObject heaterBed = status["heater_bed"];
  JsonObject virtualSdCard = status["virtual_sdcard"];

  const JsonVariantConst powerValue = purifier["power_detected"];
  const JsonVariantConst modeValue = purifier["mode"];
  const JsonVariantConst targetValue = purifier["desired_chamber_temp"];
  const JsonVariantConst cavityValue = cavity["temperature"];
  const JsonVariantConst innerSpeedValue = innerFan["speed"];
  const JsonVariantConst exhaustSpeedValue = exhaustFan["speed"];
  const JsonVariantConst bedTargetValue = heaterBed["target"];
  const JsonVariantConst sdFileActiveValue = virtualSdCard["is_active"];
  const char *stateValue = printStats["state"];

  // Safety-critical fields must be present and plausible; missing JSON values
  // are rejected rather than silently interpreted as zero.
  float modeNumber = 0.0f;
  float targetC = 0.0f;
  float chamberC = 0.0f;
  float bedTargetC = 0.0f;
  float innerFanPercent = 0.0f;
  float exhaustFanPercent = 0.0f;
  const bool requiredObjectsPresent =
      result && status && purifier && exhaustFan && innerFan && cavity &&
      printStats;
  const bool requiredFieldsValid =
      requiredObjectsPresent && powerValue.is<bool>() &&
      readNumber(modeValue, modeNumber) && floorf(modeNumber) == modeNumber &&
      modeNumber >= 0.0f && modeNumber <= 3.0f &&
      readNumber(targetValue, targetC) && targetC >= 0.0f &&
      targetC <= 100.0f && readNumber(cavityValue, chamberC) &&
      chamberC >= -20.0f && chamberC <= 150.0f &&
      normalizeFanSpeed(innerSpeedValue, innerFanPercent) &&
      normalizeFanSpeed(exhaustSpeedValue, exhaustFanPercent) &&
      isKnownPrintState(stateValue);
  const bool heaterBedTargetValid =
      readNumber(bedTargetValue, bedTargetC) && bedTargetC >= 0.0f &&
      bedTargetC <= 150.0f;
  const bool sdFileActivityValid = sdFileActiveValue.is<bool>();
  const bool sdFileActive =
      sdFileActivityValid && sdFileActiveValue.as<bool>();

  if (!requiredFieldsValid) {
    telemetryInvalid = true;
    telemetry.valid = false;
    policyState.heaterOn = false;
    policyState.blowerRequested = false;
    policyState.lastValidModeKnown = false;
    policyState.idleBedPreheatActive = false;
    policyState.cancelBlowerCooldown();
    inhibitReason = u1heater::InhibitReason::INVALID_TELEMETRY;
    setHeater(false);
    setBlower(false);
    linkState = LinkState::INVALID;
    Serial.println("INVALID TELEMETRY: required field missing or out of range; heater OFF");
    Serial.println("PARSED: invalid safety-critical fields");
    Serial.println("MOONRAKER RAW:");
    Serial.println(payload);
    debugSnapshot();
    return;
  }

  printState = stateValue;
  printProgress = 0.0f;
  progressValid = false;
  float progress = 0.0f;
  if (readNumber(status["virtual_sdcard"]["progress"], progress) &&
      progress >= 0.0f && progress <= 1.0f) {
    printProgress = progress;
    progressValid = true;
  }

  const int mode = static_cast<int>(modeNumber);
  // Treat terminal prints with a cooling mode or exhaust request as venting.
  const bool postPrintVenting =
      isTerminalPrintState(stateValue) &&
      (mode == 1 || exhaustFanPercent > 0.0f);
  telemetry = {
      true,
      powerValue.as<bool>(),
      mode,
      targetC,
      chamberC,
      heaterBedTargetValid,
      bedTargetC,
      strcmp(stateValue, "standby") == 0,
      sdFileActivityValid,
      sdFileActive,
      innerFanPercent,
      exhaustFanPercent,
      postPrintVenting};

  telemetryEverValid = true;
  telemetryInvalid = false;
  telemetryTransportLost = false;
  lastValidPollTime = millis();
  linkState = LinkState::OK;

  const u1heater::Decision decision =
#if defined(U1_BENCH_TEST) && U1_BENCH_TEST
      u1heater::decide(telemetry, policyState, true, 0.0f,
                       lastValidPollTime);
#else
      u1heater::decide(telemetry, policyState, pwmSignalStable(exhaustPwm),
                       exhaustPwm.activeDuty, lastValidPollTime);
#endif
  inhibitReason = decision.reason;
  setBlower(decision.blowerOn);
  setHeater(decision.heaterOn);
  debugTelemetry(payload);
}

void forceOutputsOff(u1heater::InhibitReason reason) {
  if (reason == u1heater::InhibitReason::TELEMETRY_TIMEOUT) {
    const u1heater::Decision decision =
        u1heater::handleTelemetryTimeout(policyState, millis());
    inhibitReason = decision.reason;
    setHeater(decision.heaterOn);
    setBlower(decision.blowerOn);
    return;
  }

  policyState.heaterOn = false;
  policyState.blowerRequested = false;
  policyState.lastValidModeKnown = false;
  policyState.idleBedPreheatActive = false;
  policyState.cancelBlowerCooldown();
  inhibitReason = reason;
  setHeater(false);
  setBlower(false);
}

void handleTransportFailure(int httpCode) {
  telemetryTransportLost = true;
  linkState = telemetryEverValid ? LinkState::LOST : LinkState::WAITING;
  Serial.printf("Moonraker transport failure: HTTP %d\n", httpCode);
  if (!telemetryEverValid) {
    forceOutputsOff(u1heater::InhibitReason::INVALID_TELEMETRY);
  }
  debugSnapshot();
}

void handleInvalidResponse(const char *reason, const String &payload) {
  telemetryInvalid = true;
  telemetry.valid = false;
  linkState = LinkState::INVALID;
  forceOutputsOff(u1heater::InhibitReason::INVALID_TELEMETRY);
  Serial.printf("Moonraker response rejected: %s; heater OFF\n", reason);
  Serial.println("MOONRAKER RAW:");
  Serial.println(payload);
  Serial.println("PARSED: invalid JSON");
  debugSnapshot();
}

void pollMoonraker() {
  HTTPClient http;
  const String url =
      String("http://") + U1_IP_ADDRESS +
      "/printer/objects/query?print_stats&virtual_sdcard&temperature_sensor%20cavity&heater_bed&purifier";
  if (!http.begin(url)) {
    handleTransportFailure(-1);
    return;
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  const int httpCode = http.GET();
  if (httpCode != 200) {
    http.end();
    // Transport/server failures get the grace period; client errors fail off.
    if (httpCode < 0 || httpCode >= 500) {
      handleTransportFailure(httpCode);
    } else {
      handleInvalidResponse("unexpected HTTP status", String(httpCode));
    }
    return;
  }

  const String payload = http.getString();
  http.end();

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, payload);
  if (error) {
    handleInvalidResponse(error.c_str(), payload);
    return;
  }
  applyTelemetry(payload, document);
}

void serviceTelemetryTimeouts(uint32_t now) {
  if (policyState.preheatTimerStarted &&
      u1heater::enforcePreheatTimeout(policyState, now)) {
    inhibitReason = u1heater::InhibitReason::PREHEAT_TIMEOUT;
    setHeater(false);
  }

  if (u1heater::enforceIdleBedPreheatTimeout(policyState, now)) {
    const bool heaterWasOn = policyState.heaterOn;
    policyState.heaterOn = false;
    policyState.idleBedPreheatActive = false;
    policyState.blowerRequested = false;
    if (heaterWasOn) {
      policyState.startBlowerCooldown(now);
    }
    inhibitReason = u1heater::InhibitReason::PREHEAT_TIMEOUT;
    setHeater(false);
    setBlower(policyState.blowerCooldownActive);
  }

  if (!telemetryEverValid || telemetryInvalid) {
    return;
  }
  const uint32_t elapsed = now - lastValidPollTime;
  // Keep the last command during transport grace, then force the heater off.
  if (elapsed >= TELEMETRY_GRACE_MS) {
    telemetryTransportLost = true;
    linkState = LinkState::EXPIRED;
    forceOutputsOff(u1heater::InhibitReason::TELEMETRY_TIMEOUT);
  } else if (telemetryTransportLost) {
    linkState = LinkState::LOST;
  }
}

void serviceBlowerCooldown(uint32_t now) {
  if (u1heater::expireBlowerCooldown(policyState, now)) {
    setBlower(policyState.blowerRequested);
  }
}

// Smooth the noisy on-chip sensor for display only; it is not a safety input.
float readFilteredCpuTemperature() {
  const float sample = temperatureRead();
  if (!isfinite(sample)) {
    return NAN;
  }

  if (!cpuTempFilterInitialized) {
    filteredCpuTempC = sample;
    cpuTempFilterInitialized = true;
  } else {
    filteredCpuTempC +=
        CPU_TEMP_FILTER_ALPHA * (sample - filteredCpuTempC);
  }
  return filteredCpuTempC;
}

void startIpSplashWhenConnected(uint32_t now) {
  if (!ipSplashStarted && WiFi.status() == WL_CONNECTED) {
    ipSplashStarted = true;
    ipSplashStartedAt = now;
    startupIpAddress = WiFi.localIP().toString();
  }
}

void updateIpSplash(uint32_t now) {
  const uint32_t elapsed = now - ipSplashStartedAt;
  const uint32_t remainingMs = IP_SPLASH_DURATION_MS - elapsed;
  const uint32_t secondsRemaining = (remainingMs + 999) / 1000;
  const String countdown = String("Normal in ") + secondsRemaining + "s";
  const int16_t ipWidth = startupIpAddress.length() * 6;
  const int16_t countdownWidth = countdown.length() * 6;

  display.clearDisplay();
  display.setTextWrap(false);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(16, 4);
  display.print("U1 CONTROLLER IP");
  display.drawLine(0, 15, SCREEN_WIDTH, 15, SSD1306_WHITE);
  display.setCursor((SCREEN_WIDTH - ipWidth) / 2, 27);
  display.print(startupIpAddress);
  display.drawLine(0, 42, SCREEN_WIDTH, 42, SSD1306_WHITE);
  display.setCursor((SCREEN_WIDTH - countdownWidth) / 2, 51);
  display.print(countdown);
  display.display();
}

void updateDisplay() {
  if (!displayAvailable) {
    return;
  }

  const uint32_t now = millis();
  if (!ipSplashStarted && WiFi.status() == WL_CONNECTED) {
    startIpSplashWhenConnected(now);
  }
  if (!ipSplashStarted) {
    display.clearDisplay();
    display.setTextWrap(false);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(16, 4);
    display.print("U1 CONTROLLER");
    display.drawLine(0, 15, SCREEN_WIDTH, 15, SSD1306_WHITE);
    display.setCursor(1, 28);
    display.print("Connecting to WiFi...");
    display.display();
    return;
  }
  if (ipSplashStarted && now - ipSplashStartedAt < IP_SPLASH_DURATION_MS) {
    updateIpSplash(now);
    return;
  }

  display.clearDisplay();
  display.setTextWrap(false);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 2);
  if (telemetry.valid) {
    if (policyState.idleBedPreheatActive) {
      display.print("U1 IDLE HEAT");
    } else {
      switch (telemetry.mode) {
        case 0: display.print("U1 IDLE"); break;
        case 1: display.print("U1 COOLING"); break;
        case 2: display.print("U1 PREHEAT"); break;
        case 3: display.print("U1 HOT CHMBR"); break;
        default: display.print("U1 UNKNOWN"); break;
      }
    }
  } else {
    display.print("U1 WAIT");
  }
  if (printState == "printing" && progressValid) {
    const int percent = static_cast<int>(printProgress * 100.0f);
    display.setCursor(78, 2);
    display.print(percent);
    display.print("%");
    display.drawRect(104, 2, 23, 7, SSD1306_WHITE);
    const int fillWidth = map(percent, 0, 100, 0, 19);
    if (fillWidth > 0) {
      display.fillRect(106, 4, fillWidth, 3, SSD1306_WHITE);
    }
  } else {
    display.setCursor(104, 2);
    display.print(WiFi.status() == WL_CONNECTED ? "WiFi" : "--");
  }
  display.drawLine(0, 12, 128, 12, SSD1306_WHITE);
  display.drawLine(63, 15, 63, 62, SSD1306_WHITE);

  display.setCursor(0, 17);
  display.print("CHBR: ");
  if (telemetryEverValid) {
    display.print(telemetry.chamberC, 0);
    display.print("C");
  } else {
    display.print("--C");
  }
  display.setCursor(67, 17);
  display.print("TRGT: ");
  if (telemetryEverValid) {
    display.print(telemetry.targetC, 0);
    display.print("C");
  } else {
    display.print("--C");
  }

  display.setCursor(0, 28);
  display.print(currentHeaterOn ? "HTR: ON" : "HTR: OFF");
  display.setCursor(67, 28);
  display.print(currentBlowerOn ? "RCLN: ON" : "RCLN: OFF");

  display.setCursor(0, 39);
  display.print(valveOpen ? "VLV: OPEN" : "VLV: CLOSE");
  display.setCursor(67, 39);
  // Show the parsed U1 request here; GPIO 26's measured duty is logged on serial.
  if (telemetry.valid) {
    display.print("EXST: ");
    display.print(telemetry.exhaustFanPercent, 0);
    display.print("%");
  } else {
    display.print("EXST: --");
  }

  display.setCursor(0, 50);
  switch (linkState) {
    case LinkState::WAITING: display.print("TMRY: WAIT"); break;
    case LinkState::OK: display.print("TMRY: OK"); break;
    case LinkState::LOST: display.print("TMRY: LOST"); break;
    case LinkState::EXPIRED: display.print("TMRY: OFF"); break;
    case LinkState::INVALID: display.print("TMRY: BAD"); break;
  }
  display.setCursor(67, 50);
  display.print("CPU: ");
  const float cpuTemperatureC = readFilteredCpuTemperature();
  if (isfinite(cpuTemperatureC)) {
    display.print(cpuTemperatureC, 0);
    display.print("C");
  } else {
    display.print("--C");
  }
  display.display();
}

void printPwmDebug(const char *name, const PwmState &state) {
  Serial.printf("%s valid=%d active=%.1f%% raw=%.1f%% Hz=%.1f\n", name,
                state.valid, state.activeDuty, state.rawDuty,
                state.frequencyHz);
}

void setup() {
  Serial.begin(115200);

#if defined(U1_BENCH_TEST) && U1_BENCH_TEST
  Serial.println(
      "BENCH TEST: GPIO26 heater inhibit bypassed; PTC must be disconnected.");
#endif
  Serial.printf("HEATER relay polarity=%s\n",
                HEATER_RELAY_ACTIVE_LOW ? "active-low" : "active-high");

  // Set the configured OFF level before enabling the relay output.
  digitalWrite(PIN_HEATER_RELAY, HEATER_RELAY_OFF_LEVEL);
  pinMode(PIN_HEATER_RELAY, OUTPUT);
  digitalWrite(PIN_BLOWER, LOW);
  pinMode(PIN_BLOWER, OUTPUT);
  pinMode(PIN_EXHAUST_PWM_MONITOR, INPUT);
  pinMode(PIN_RECIRC_PWM_MONITOR, INPUT);

  const int servoChannel = flapServo.attach(PIN_SERVO);
  flapServo.write(SERVO_OPEN_DEG);
  currentServoAngle = SERVO_OPEN_DEG;
  valveOpen = true;
  Serial.printf("SERVO attached=%d channel=%d boot_angle=%u pulse=%d us\n",
                flapServo.attached(), servoChannel,
                static_cast<unsigned int>(currentServoAngle),
                flapServo.readMicroseconds());

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  displayAvailable = display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR);
  if (displayAvailable) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 4);
    display.println("U1 CONTROLLER");
    display.drawLine(0, 14, SCREEN_WIDTH, 14, SSD1306_WHITE);
    display.setCursor(0, 25);
    display.println("HEATER: OFF");
    display.println("WAITING FOR U1");
    display.display();
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  statusServer.on("/api/status", HTTP_GET, handleStatusRequest);
  statusServer.onNotFound(handleStatusNotFound);
  statusServer.begin();
  Serial.println("U1-driven chamber controller starting; heater OFF");
}

void loop() {
  const uint32_t now = millis();
  startIpSplashWhenConnected(now);

  // GPIO 26 participates in valve/heater safety; GPIO 27 is diagnostic only.
  updatePwmState(exhaustPwm,
                 readPwm(PIN_EXHAUST_PWM_MONITOR, true), now);
  updatePwmState(recircPwm,
                 readPwm(PIN_RECIRC_PWM_MONITOR, false), now);
  updateValveCommand(now);
#if !defined(U1_BENCH_TEST) || !U1_BENCH_TEST
  if (telemetry.valid &&
      (!pwmSignalStable(exhaustPwm) ||
       exhaustPwm.activeDuty >=
           u1heater::EXHAUST_HEAT_INHIBIT_PERCENT)) {
    policyState.heaterOn = false;
    inhibitReason = pwmSignalStable(exhaustPwm)
                        ? u1heater::InhibitReason::EXHAUST_REQUEST_HIGH
                        : u1heater::InhibitReason::EXHAUST_SIGNAL_UNKNOWN;
    setHeater(false);
  }
#endif

  if (WiFi.status() != WL_CONNECTED &&
      (lastReconnectAttempt == 0 ||
       now - lastReconnectAttempt >= RECONNECT_INTERVAL_MS)) {
    lastReconnectAttempt = now;
    WiFi.reconnect();
    Serial.println("Wi-Fi reconnect requested");
  }

  if (!pollScheduled || now - lastPollTime >= POLL_INTERVAL_MS) {
    pollScheduled = true;
    lastPollTime = now;
    if (WiFi.status() == WL_CONNECTED) {
      pollMoonraker();
    } else {
      handleTransportFailure(0);
    }
  }

  serviceTelemetryTimeouts(millis());
  serviceBlowerCooldown(millis());

  const uint32_t displayNow = millis();
  if (displayNow - lastDisplayTime >= DISPLAY_INTERVAL_MS) {
    lastDisplayTime = displayNow;
    updateDisplay();
  }
  statusServer.handleClient();
  delay(5);
}
