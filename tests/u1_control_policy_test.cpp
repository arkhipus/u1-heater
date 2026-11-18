#include "../arduino/esp32_heater.cpp/u1_control_policy.h"

#include <assert.h>
#include <stdio.h>

using namespace u1heater;

static Telemetry eligibleTelemetry() {
  Telemetry telemetry = {};
  telemetry.valid = true;
  telemetry.powerDetected = true;
  telemetry.mode = 3;
  telemetry.targetC = 45.0f;
  telemetry.chamberC = 42.0f;
  telemetry.innerFanPercent = 60.0f;
  return telemetry;
}

static Telemetry idleBedPreheatTelemetry() {
  Telemetry telemetry = eligibleTelemetry();
  telemetry.mode = 0;
  telemetry.targetC = 0.0f;
  telemetry.chamberC = 47.0f;
  telemetry.innerFanPercent = 0.0f;
  telemetry.heaterBedTargetValid = true;
  telemetry.heaterBedTargetC = 100.0f;
  telemetry.printStandby = true;
  telemetry.sdFileActivityValid = true;
  return telemetry;
}

static Decision evaluate(Telemetry telemetry, PolicyState &state,
                         uint32_t now) {
  return decide(telemetry, state, true, 0.0f, now);
}

int main() {
  assert(valveFallbackForUnavailableTelemetry(true, 0.0f) ==
         ValveFallbackAction::HOLD_LAST);
  assert(valveFallbackForUnavailableTelemetry(true, 1.0f) ==
         ValveFallbackAction::OPEN);
  assert(valveFallbackForUnavailableTelemetry(false, 0.0f) ==
         ValveFallbackAction::OPEN);

  assert(isStaticInputConfirmed(true, 3, 3, false, false));
  assert(isStaticInputConfirmed(true, 3, 3, true, true));
  assert(!isStaticInputConfirmed(true, 3, 3, false, true));
  assert(!isStaticInputConfirmed(true, 2, 3, false, false));
  assert(!isStaticInputConfirmed(false, 3, 3, false, false));

  PolicyState state;
  Telemetry telemetry = eligibleTelemetry();

  Decision decision = evaluate(telemetry, state, 0);
  assert(decision.heaterOn);
  assert(decision.blowerOn);

  telemetry.chamberC = 44.0f;
  decision = evaluate(telemetry, state, 1000);
  assert(decision.heaterOn);

  telemetry.chamberC = 45.0f;
  decision = evaluate(telemetry, state, 2000);
  assert(!decision.heaterOn);

  telemetry = eligibleTelemetry();
  telemetry.exhaustFanPercent = 20.0f;
  decision = evaluate(telemetry, state, 3000);
  assert(!decision.heaterOn);
  assert(decision.reason == InhibitReason::EXHAUST_REQUEST_HIGH);

  telemetry = eligibleTelemetry();
  decision = decide(telemetry, state, true, 20.0f, 4000);
  assert(!decision.heaterOn);
  assert(decision.reason == InhibitReason::EXHAUST_REQUEST_HIGH);

  telemetry = eligibleTelemetry();
  telemetry.powerDetected = false;
  decision = evaluate(telemetry, state, 5000);
  assert(!decision.heaterOn);
  assert(decision.reason == InhibitReason::U1_POWER_UNDETECTED);

  telemetry = eligibleTelemetry();
  telemetry.mode = 1;
  decision = evaluate(telemetry, state, 6000);
  assert(!decision.heaterOn);
  assert(decision.reason == InhibitReason::MODE_INELIGIBLE);

  telemetry = eligibleTelemetry();
  telemetry.innerFanPercent = 0.0f;
  decision = evaluate(telemetry, state, 7000);
  assert(!decision.heaterOn);
  assert(decision.reason == InhibitReason::BLOWER_REQUEST_OFF);

  telemetry = eligibleTelemetry();
  decision = decide(telemetry, state, false, 0.0f, 7500);
  assert(!decision.heaterOn);
  assert(decision.reason == InhibitReason::EXHAUST_SIGNAL_UNKNOWN);

  telemetry = eligibleTelemetry();
  telemetry.postPrintVenting = true;
  decision = evaluate(telemetry, state, 8000);
  assert(!decision.heaterOn);
  assert(decision.reason == InhibitReason::POST_PRINT_VENTING);

  telemetry = eligibleTelemetry();
  telemetry.targetC = 101.0f;
  decision = evaluate(telemetry, state, 9000);
  assert(!decision.heaterOn);
  assert(decision.reason == InhibitReason::INVALID_TARGET);

  telemetry = eligibleTelemetry();
  telemetry.chamberC = 151.0f;
  decision = evaluate(telemetry, state, 9500);
  assert(!decision.heaterOn);
  assert(decision.reason == InhibitReason::INVALID_CHAMBER_TEMPERATURE);

  telemetry = eligibleTelemetry();
  telemetry.mode = 2;
  decision = evaluate(telemetry, state, 10000);
  assert(decision.heaterOn);
  decision = evaluate(telemetry, state, 10000 + PREHEAT_LIMIT_MS);
  assert(!decision.heaterOn);
  assert(decision.reason == InhibitReason::PREHEAT_TIMEOUT);
  decision = evaluate(telemetry, state, 11000 + PREHEAT_LIMIT_MS);
  assert(!decision.heaterOn);
  assert(decision.reason == InhibitReason::PREHEAT_TIMEOUT);

  telemetry.mode = 3;
  decision = evaluate(telemetry, state, 12000 + PREHEAT_LIMIT_MS);
  assert(decision.heaterOn);

  telemetry.mode = 2;
  decision = evaluate(telemetry, state, 13000 + PREHEAT_LIMIT_MS);
  assert(decision.heaterOn);
  decision = evaluate(telemetry, state,
                      13000 + 2 * PREHEAT_LIMIT_MS);
  assert(!decision.heaterOn);
  assert(decision.reason == InhibitReason::PREHEAT_TIMEOUT);

  telemetry.valid = false;
  decision = evaluate(telemetry, state, 14000 + 2 * PREHEAT_LIMIT_MS);
  assert(!decision.heaterOn);
  assert(decision.reason == InhibitReason::INVALID_TELEMETRY);

  PolicyState cooldownState;
  Telemetry cooldownTelemetry = eligibleTelemetry();
  decision = evaluate(cooldownTelemetry, cooldownState, 0);
  assert(decision.heaterOn);

  cooldownTelemetry.mode = 0;
  cooldownTelemetry.innerFanPercent = 0.0f;
  decision = evaluate(cooldownTelemetry, cooldownState, 1000);
  assert(!decision.heaterOn);
  assert(decision.blowerOn);
  assert(cooldownState.blowerCooldownActive);

  cooldownTelemetry.mode = 3;
  decision = evaluate(cooldownTelemetry, cooldownState, 2000);
  assert(!decision.heaterOn);
  assert(decision.blowerOn);

  cooldownTelemetry.mode = 0;
  decision = evaluate(cooldownTelemetry, cooldownState,
                      1000 + BLOWER_COOLDOWN_MS - 1);
  assert(decision.blowerOn);
  decision = evaluate(cooldownTelemetry, cooldownState,
                      1000 + BLOWER_COOLDOWN_MS);
  assert(!decision.blowerOn);
  assert(!cooldownState.blowerCooldownActive);

  PolicyState alreadyOffState;
  Telemetry alreadyOffTelemetry = eligibleTelemetry();
  alreadyOffTelemetry.chamberC = alreadyOffTelemetry.targetC;
  decision = evaluate(alreadyOffTelemetry, alreadyOffState, 0);
  assert(!decision.heaterOn);
  alreadyOffTelemetry.mode = 0;
  alreadyOffTelemetry.innerFanPercent = 0.0f;
  decision = evaluate(alreadyOffTelemetry, alreadyOffState, 1000);
  assert(!decision.blowerOn);
  assert(!alreadyOffState.blowerCooldownActive);

  PolicyState thermostatOffState;
  Telemetry thermostatOffTelemetry = eligibleTelemetry();
  decision = evaluate(thermostatOffTelemetry, thermostatOffState, 0);
  assert(decision.heaterOn);
  thermostatOffTelemetry.chamberC = thermostatOffTelemetry.targetC;
  decision = evaluate(thermostatOffTelemetry, thermostatOffState, 1000);
  assert(!decision.heaterOn);
  assert(decision.blowerOn);
  assert(!thermostatOffState.blowerCooldownActive);

  PolicyState invalidCooldownState;
  Telemetry invalidCooldownTelemetry = eligibleTelemetry();
  decision = evaluate(invalidCooldownTelemetry, invalidCooldownState, 0);
  assert(decision.heaterOn);
  invalidCooldownTelemetry.mode = 0;
  invalidCooldownTelemetry.innerFanPercent = 0.0f;
  decision = evaluate(invalidCooldownTelemetry, invalidCooldownState, 1000);
  assert(decision.blowerOn);
  invalidCooldownTelemetry.valid = false;
  decision = evaluate(invalidCooldownTelemetry, invalidCooldownState, 2000);
  assert(!decision.blowerOn);
  assert(!invalidCooldownState.blowerCooldownActive);

  PolicyState timeoutState;
  timeoutState.heaterOn = true;
  timeoutState.blowerRequested = true;
  decision = handleTelemetryTimeout(timeoutState, 5000);
  assert(!decision.heaterOn);
  assert(decision.blowerOn);
  assert(!timeoutState.heaterOn);
  assert(timeoutState.blowerCooldownActive);
  assert(!expireBlowerCooldown(timeoutState,
                               5000 + BLOWER_COOLDOWN_MS - 1));
  assert(expireBlowerCooldown(timeoutState, 5000 + BLOWER_COOLDOWN_MS));
  assert(!timeoutState.blowerRequested);

  Telemetry bedTelemetry = idleBedPreheatTelemetry();
  assert(isIdleBedPreheatRequested(bedTelemetry));
  PolicyState bedState;
  decision = evaluate(bedTelemetry, bedState, 0);
  assert(decision.heaterOn);
  assert(decision.blowerOn);
  bedTelemetry.chamberC = 49.0f;
  decision = evaluate(bedTelemetry, bedState, 1000);
  assert(decision.heaterOn);
  bedTelemetry.chamberC = IDLE_BED_PREHEAT_TARGET_C;
  decision = evaluate(bedTelemetry, bedState, 2000);
  assert(!decision.heaterOn);

  Telemetry gatedBedTelemetry = idleBedPreheatTelemetry();
  gatedBedTelemetry.heaterBedTargetC = 94.9f;
  assert(!isIdleBedPreheatRequested(gatedBedTelemetry));
  gatedBedTelemetry = idleBedPreheatTelemetry();
  gatedBedTelemetry.heaterBedTargetValid = false;
  assert(!isIdleBedPreheatRequested(gatedBedTelemetry));
  gatedBedTelemetry = idleBedPreheatTelemetry();
  gatedBedTelemetry.printStandby = false;
  assert(!isIdleBedPreheatRequested(gatedBedTelemetry));
  gatedBedTelemetry = idleBedPreheatTelemetry();
  gatedBedTelemetry.sdFileActivityValid = false;
  assert(!isIdleBedPreheatRequested(gatedBedTelemetry));
  gatedBedTelemetry = idleBedPreheatTelemetry();
  gatedBedTelemetry.sdFileActive = true;
  assert(!isIdleBedPreheatRequested(gatedBedTelemetry));
  gatedBedTelemetry = idleBedPreheatTelemetry();
  gatedBedTelemetry.powerDetected = false;
  assert(!isIdleBedPreheatRequested(gatedBedTelemetry));
  gatedBedTelemetry = idleBedPreheatTelemetry();
  gatedBedTelemetry.mode = 1;
  assert(!isIdleBedPreheatRequested(gatedBedTelemetry));

  PolicyState bedTimeoutState;
  bedTelemetry = idleBedPreheatTelemetry();
  decision = evaluate(bedTelemetry, bedTimeoutState, 1000);
  assert(decision.heaterOn);
  decision = evaluate(bedTelemetry, bedTimeoutState,
                      1000 + PREHEAT_LIMIT_MS);
  assert(!decision.heaterOn);
  assert(decision.blowerOn);
  assert(decision.reason == InhibitReason::PREHEAT_TIMEOUT);
  assert(bedTimeoutState.idleBedPreheatExpired);
  decision = evaluate(bedTelemetry, bedTimeoutState,
                      1001 + PREHEAT_LIMIT_MS);
  assert(!decision.heaterOn);
  assert(!bedTimeoutState.heaterOn);
  bedTelemetry.heaterBedTargetC = 94.0f;
  evaluate(bedTelemetry, bedTimeoutState, 2000 + PREHEAT_LIMIT_MS);
  assert(!bedTimeoutState.idleBedPreheatExpired);
  bedTelemetry.heaterBedTargetC = 100.0f;
  bedTelemetry.chamberC = 47.0f;
  decision = evaluate(bedTelemetry, bedTimeoutState,
                      2001 + PREHEAT_LIMIT_MS);
  assert(decision.heaterOn);

  PolicyState bedAfterrunState;
  bedTelemetry = idleBedPreheatTelemetry();
  decision = evaluate(bedTelemetry, bedAfterrunState, 0);
  assert(decision.heaterOn);
  bedTelemetry.printStandby = false;
  decision = evaluate(bedTelemetry, bedAfterrunState, 1000);
  assert(!decision.heaterOn);
  assert(decision.blowerOn);
  assert(bedAfterrunState.blowerCooldownActive);
  decision = evaluate(bedTelemetry, bedAfterrunState,
                      1000 + BLOWER_COOLDOWN_MS);
  assert(!decision.blowerOn);

  PolicyState bedHandoffState;
  bedTelemetry = idleBedPreheatTelemetry();
  decision = evaluate(bedTelemetry, bedHandoffState, 0);
  assert(decision.heaterOn);
  bedTelemetry.mode = 3;
  bedTelemetry.targetC = 45.0f;
  bedTelemetry.chamberC = 42.0f;
  bedTelemetry.innerFanPercent = 60.0f;
  bedTelemetry.printStandby = false;
  bedTelemetry.sdFileActive = true;
  decision = evaluate(bedTelemetry, bedHandoffState, 1000);
  assert(decision.heaterOn);
  assert(decision.blowerOn);
  assert(!bedHandoffState.blowerCooldownActive);

  PolicyState bedExhaustState;
  bedTelemetry = idleBedPreheatTelemetry();
  bedTelemetry.exhaustFanPercent = EXHAUST_HEAT_INHIBIT_PERCENT;
  decision = evaluate(bedTelemetry, bedExhaustState, 0);
  assert(!decision.heaterOn);
  assert(decision.blowerOn);
  assert(decision.reason == InhibitReason::EXHAUST_REQUEST_HIGH);

  puts("U1 heater control policy tests passed");
  return 0;
}
