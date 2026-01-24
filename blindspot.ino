// Dual HB100 + RCWL blind spot detector

const unsigned long SAMPLE_PERIOD_US = 200;   // 5 kHz
const int MIN_AMPLITUDE = 70; // ADC counts

const unsigned long MIN_PERIOD_US = 1000;
const unsigned long MAX_PERIOD_US = 500000;

const unsigned long MOTION_HOLD_MS = 800;

const int EVENTS_TO_TRIGGER = 2;
const unsigned long EVENT_WINDOW_MS = 500;

const unsigned long RCWL_MIN_ACTIVE_MS = 2000;

// Pins
const int HB100_LEFT_PIN = A0;
const int HB100_RIGHT_PIN = A1;

const int RCWL_LEFT_PIN = 2;
const int RCWL_RIGHT_PIN = 3;

const int LED_LEFT_PIN = 8;
const int LED_RIGHT_PIN = 9;

bool ENABLE_RCWL_LEFT = true;
bool ENABLE_RCWL_RIGHT = true;

// Timing
unsigned long lastSample = 0;

// LEFT
long baselineLeft = 512;
bool lastSignLeft = false;
unsigned long lastCrossLeft = 0;
unsigned long expireLeft = 0;
int eventsLeft = 0;
unsigned long windowLeft  = 0;
unsigned long rcwlLeftStart = 0;

// RIGHT
long baselineRight = 512;
bool lastSignRight = false;
unsigned long lastCrossRight = 0;
unsigned long expireRight = 0;
int eventsRight = 0;
unsigned long windowRight = 0;
unsigned long rcwlRightStart = 0;

void setup() {
  analogReference(DEFAULT);

  pinMode(HB100_LEFT_PIN, INPUT);
  pinMode(HB100_RIGHT_PIN, INPUT);

  pinMode(RCWL_LEFT_PIN, INPUT);
  pinMode(RCWL_RIGHT_PIN, INPUT);

  pinMode(LED_LEFT_PIN, OUTPUT);
  pinMode(LED_RIGHT_PIN, OUTPUT);

  digitalWrite(LED_LEFT_PIN, HIGH);
  digitalWrite(LED_RIGHT_PIN, HIGH);
}

// RCWL override
bool rcwlOverride(int pin, unsigned long &startMs, unsigned long nowMs, bool enabled) {
  if (!enabled) {
    startMs = 0;
    return false;
  }

  bool active = digitalRead(pin) == HIGH;

  if (active) {
    if (startMs == 0) startMs = nowMs;
  } else {
    startMs = 0;
  }

  return (startMs != 0 && (nowMs - startMs >= RCWL_MIN_ACTIVE_MS));
}

// HB100 processing
bool processHb(
  int raw,
  long &baseline,
  bool &lastSign,
  unsigned long &lastCross,
  unsigned long &expireMs,
  int &eventCount,
  unsigned long &windowStart,
  unsigned long nowMicros,
  unsigned long nowMs
) {
  baseline += (raw - baseline) >> 6;

  int val = raw - baseline;
  bool sign = (val > 0);

  bool valid = false;

  if (!lastSign && sign && abs(val) > MIN_AMPLITUDE) {
    if (lastCross != 0) {
      unsigned long period = nowMicros - lastCross;
      if (period > MIN_PERIOD_US && period < MAX_PERIOD_US) {
        valid = true;
      }
    }
    lastCross = nowMicros;
  }

  lastSign = sign;

  if (valid) {
    if (eventCount == 0) {
      windowStart = nowMs;
      eventCount = 1;
    } else if (nowMs - windowStart <= EVENT_WINDOW_MS) {
      eventCount++;
    } else {
      windowStart = nowMs;
      eventCount = 1;
    }

    if (eventCount >= EVENTS_TO_TRIGGER) {
      expireMs = nowMs + MOTION_HOLD_MS;
      eventCount = 0;
    }
  } else if (eventCount && nowMs - windowStart > EVENT_WINDOW_MS) {
    eventCount = 0;
  }

  return (nowMs < expireMs);
}

void loop() {
  unsigned long nowMicros = micros();
  if (nowMicros - lastSample < SAMPLE_PERIOD_US) return;
  lastSample += SAMPLE_PERIOD_US;

  unsigned long nowMs = millis();

  // ADC settle
  analogRead(HB100_LEFT_PIN);
  int rawLeft = analogRead(HB100_LEFT_PIN);

  analogRead(HB100_RIGHT_PIN);
  int rawRight = analogRead(HB100_RIGHT_PIN);

  bool hb100DetectedMotionLeft = processHb(
    rawLeft,
    baselineLeft,
    lastSignLeft,
    lastCrossLeft,
    expireLeft,
    eventsLeft,
    windowLeft,
    nowMicros,
    nowMs
  );

  bool hb100DetectedMotionRight = processHb(
    rawRight,
    baselineRight,
    lastSignRight,
    lastCrossRight,
    expireRight,
    eventsRight,
    windowRight,
    nowMicros,
    nowMs
  );

  bool rcwlIsOverrideLeft  = rcwlOverride(RCWL_LEFT_PIN,  rcwlLeftStart,  nowMs, ENABLE_RCWL_LEFT);
  bool rcwlIsOverrideRight = rcwlOverride(RCWL_RIGHT_PIN, rcwlRightStart, nowMs, ENABLE_RCWL_RIGHT);

  // RCWL ALWAYS override
  digitalWrite(LED_LEFT_PIN,  (hb100DetectedMotionLeft  && !rcwlIsOverrideLeft)  ? LOW : HIGH);
  digitalWrite(LED_RIGHT_PIN, (hb100DetectedMotionRight && !rcwlIsOverrideRight) ? LOW : HIGH);
}
