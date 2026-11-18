#include <Arduino.h>
#include <ESP32Servo.h>

constexpr uint8_t PIN_SERVO = 18;
constexpr uint8_t PIN_BLOWER = 23;
constexpr uint8_t PIN_HEATER_RELAY = 25;
constexpr uint32_t ENDPOINT_HOLD_MS = 2000;

Servo flapServo;
bool commandOpen = false;

void setup() {
  Serial.begin(115200);

  // Keep the active-high heater relay OFF during this servo-only test.
  digitalWrite(PIN_HEATER_RELAY, LOW);
  pinMode(PIN_HEATER_RELAY, OUTPUT);
  digitalWrite(PIN_BLOWER, LOW);
  pinMode(PIN_BLOWER, OUTPUT);

  const int channel = flapServo.attach(PIN_SERVO);
  Serial.printf("SERVO attached=%d channel=%d\n", flapServo.attached(),
                channel);
  if (!flapServo.attached()) {
    Serial.println("ERROR: servo attach failed");
  }
}

void loop() {
  const uint8_t angle = commandOpen ? 90 : 180;
  flapServo.write(angle);
  Serial.printf("SERVO command=%s angle=%u pulse=%d us\n",
                commandOpen ? "OPEN" : "CLOSED",
                static_cast<unsigned int>(angle),
                flapServo.readMicroseconds());
  commandOpen = !commandOpen;
  delay(ENDPOINT_HOLD_MS);
}
