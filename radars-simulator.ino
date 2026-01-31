const uint8_t HB_PIN = 10;
const uint8_t RCWL_PIN = 7;

const unsigned long CYCLE_MS = 5000;
const unsigned long HB_ON_MS = 3000;
const unsigned long RCWL_ON_START_MS = 2000;
const unsigned long RCWL_ON_END_MS   = 3000;

const unsigned long HB_HALF_PERIOD_US = 1250;

unsigned long cycleStartMs = 0;
unsigned long lastToggleUs = 0;
uint8_t hbState = LOW;

void setup() {
  pinMode(HB_PIN, OUTPUT);
  pinMode(RCWL_PIN, OUTPUT);
  digitalWrite(HB_PIN, LOW);
  digitalWrite(RCWL_PIN, LOW);
  cycleStartMs = millis();
  lastToggleUs = micros();
}

void loop() {
  unsigned long nowMs = millis();
  unsigned long nowUs = micros();

  unsigned long tMs = (unsigned long)(nowMs - cycleStartMs);
  if (tMs >= CYCLE_MS) {
    cycleStartMs = nowMs - (tMs % CYCLE_MS);
    tMs = (unsigned long)(nowMs - cycleStartMs);
    hbState = LOW;
    lastToggleUs = nowUs;
    digitalWrite(HB_PIN, LOW);
    digitalWrite(RCWL_PIN, LOW);
  }

  bool hbActive = (tMs < HB_ON_MS);
  bool rcwlActive = (tMs >= RCWL_ON_START_MS && tMs < RCWL_ON_END_MS);

  digitalWrite(RCWL_PIN, rcwlActive ? HIGH : LOW);

  if (!hbActive) {
    if (hbState != LOW) {
      hbState = LOW;
      digitalWrite(HB_PIN, LOW);
    }
    return;
  }

  if ((unsigned long)(nowUs - lastToggleUs) >= HB_HALF_PERIOD_US) {
    lastToggleUs = nowUs;
    hbState = (hbState == LOW) ? HIGH : LOW;
    digitalWrite(HB_PIN, hbState);
  }
}
