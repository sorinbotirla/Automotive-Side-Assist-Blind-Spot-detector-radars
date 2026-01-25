// Dual HB100 + RCWL blind spot detector

const unsigned long SAMPLE_PERIOD_US = 200;   // 5 kHz

const unsigned long MIN_PERIOD_US = 1000;
const unsigned long MAX_PERIOD_US = 500000;

unsigned long MOTION_HOLD_MS = 500;

int EVENTS_TO_TRIGGER = 1;
const unsigned long EVENT_WINDOW_MS = 500;

unsigned long RCWL_MIN_ACTIVE_MS = 1000;

// Pins
const int HB100_LEFT_PIN = A0;
const int HB100_RIGHT_PIN = A1;

const int RCWL_LEFT_PIN = 2;
const int RCWL_RIGHT_PIN = 3;

const int LED_LEFT_PIN = 8;
const int LED_RIGHT_PIN = 9;

bool ENABLE_RCWL_LEFT = true;
bool ENABLE_RCWL_RIGHT = true;

// Adaptive threshold settings
int MIN_AMPLITUDE_LEFT = 60;
int MIN_AMPLITUDE_RIGHT = 60;

int NOISE_MULT_LEFT = 40;     // x10 units, 40 = 4.0x
int NOISE_MULT_RIGHT = 55;

int NOISE_OFFSET_LEFT = 10;
int NOISE_OFFSET_RIGHT = 10;

int NOISE_ALPHA_SHIFT = 7;    // noiseAvg += (abs(val)-noiseAvg) >> NOISE_ALPHA_SHIFT

// Timing
unsigned long lastSample = 0;
unsigned long lastTelemetryMs = 0;

// LEFT
long baselineLeft = 512;
bool lastSignLeft = false;
unsigned long lastCrossLeft = 0;
unsigned long expireLeft = 0;
int eventsLeft = 0;
unsigned long windowLeft  = 0;
unsigned long rcwlLeftStart = 0;

long noiseAvgAbsLeft = 0;

// RIGHT
long baselineRight = 512;
bool lastSignRight = false;
unsigned long lastCrossRight = 0;
unsigned long expireRight = 0;
int eventsRight = 0;
unsigned long windowRight = 0;
unsigned long rcwlRightStart = 0;

long noiseAvgAbsRight = 0;

// Serial command parsing
String cmdLine = "";

static int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static bool strToBool(const String &s) {
  String x = s;
  x.trim();
  x.toLowerCase();
  return (x == "1" || x == "true" || x == "on" || x == "yes");
}

static void ackKV(const String &key, const String &val) {
  Serial.print("A,");
  Serial.print(key);
  Serial.print(",");
  Serial.println(val);
}

static void applySetting(const String &key, const String &val) {
  String k = key, v = val;
  k.trim();
  v.trim();

  if (k == "MOTION_HOLD_MS") {
    MOTION_HOLD_MS = (unsigned long)max(0, v.toInt());
    ackKV(k, String(MOTION_HOLD_MS));
    return;
  }

  if (k == "EVENTS_TO_TRIGGER") {
    EVENTS_TO_TRIGGER = clampInt(v.toInt(), 1, 20);
    ackKV(k, String(EVENTS_TO_TRIGGER));
    return;
  }

  if (k == "RCWL_MIN_ACTIVE_MS") {
    RCWL_MIN_ACTIVE_MS = (unsigned long)max(0, v.toInt());
    ackKV(k, String(RCWL_MIN_ACTIVE_MS));
    return;
  }

  if (k == "ENABLE_RCWL_LEFT") {
    ENABLE_RCWL_LEFT = strToBool(v);
    ackKV(k, ENABLE_RCWL_LEFT ? "1" : "0");
    return;
  }

  if (k == "ENABLE_RCWL_RIGHT") {
    ENABLE_RCWL_RIGHT = strToBool(v);
    ackKV(k, ENABLE_RCWL_RIGHT ? "1" : "0");
    return;
  }

  if (k == "MIN_AMPLITUDE_LEFT") {
    MIN_AMPLITUDE_LEFT = clampInt(v.toInt(), 0, 1023);
    ackKV(k, String(MIN_AMPLITUDE_LEFT));
    return;
  }

  if (k == "MIN_AMPLITUDE_RIGHT") {
    MIN_AMPLITUDE_RIGHT = clampInt(v.toInt(), 0, 1023);
    ackKV(k, String(MIN_AMPLITUDE_RIGHT));
    return;
  }

  if (k == "NOISE_MULT_LEFT") {
    NOISE_MULT_LEFT = clampInt(v.toInt(), 0, 300);   // 0..30.0x
    ackKV(k, String(NOISE_MULT_LEFT));
    return;
  }

  if (k == "NOISE_MULT_RIGHT") {
    NOISE_MULT_RIGHT = clampInt(v.toInt(), 0, 300);
    ackKV(k, String(NOISE_MULT_RIGHT));
    return;
  }

  if (k == "NOISE_OFFSET_LEFT") {
    NOISE_OFFSET_LEFT = clampInt(v.toInt(), -1023, 1023);
    ackKV(k, String(NOISE_OFFSET_LEFT));
    return;
  }

  if (k == "NOISE_OFFSET_RIGHT") {
    NOISE_OFFSET_RIGHT = clampInt(v.toInt(), -1023, 1023);
    ackKV(k, String(NOISE_OFFSET_RIGHT));
    return;
  }

  if (k == "NOISE_ALPHA_SHIFT") {
    NOISE_ALPHA_SHIFT = clampInt(v.toInt(), 4, 12);
    ackKV(k, String(NOISE_ALPHA_SHIFT));
    return;
  }

  ackKV("UNKNOWN", k);
}

static void handleSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;

    if (c == '\n') {
      String s = cmdLine;
      cmdLine = "";
      s.trim();
      if (s.length() == 0) continue;

      if (!s.startsWith("C,")) continue;

      int p1 = s.indexOf(',', 2);
      if (p1 < 0) continue;

      String key = s.substring(2, p1);
      String val = s.substring(p1 + 1);

      applySetting(key, val);
    } else {
      if (cmdLine.length() < 160) cmdLine += c;
    }
  }
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

static int computeAdaptiveThreshold(long noiseAvgAbs, int minAmp, int multX10, int offset) {
  long t = (noiseAvgAbs * (long)multX10) / 10L;
  t += (long)offset;
  if (t < (long)minAmp) t = (long)minAmp;
  if (t < 0) t = 0;
  if (t > 1023) t = 1023;
  return (int)t;
}

// HB100 processing
bool processHb(
  int raw,
  long &baseline,
  long &noiseAvgAbs,
  bool &lastSign,
  unsigned long &lastCross,
  unsigned long &expireMs,
  int &eventCount,
  unsigned long &windowStart,
  unsigned long nowMicros,
  unsigned long nowMs,
  int threshold
) {
  baseline += (raw - baseline) >> 6;

  int val = raw - baseline;

  int a = val;
  if (a < 0) a = -a;

  noiseAvgAbs += ((long)a - noiseAvgAbs) >> NOISE_ALPHA_SHIFT;

  bool sign = (val > 0);

  bool valid = false;

  if (!lastSign && sign && a > threshold) {
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

void setup() {
  Serial.begin(115200);

  analogReference(DEFAULT);

  pinMode(HB100_LEFT_PIN, INPUT);
  pinMode(HB100_RIGHT_PIN, INPUT);

  pinMode(RCWL_LEFT_PIN, INPUT);
  pinMode(RCWL_RIGHT_PIN, INPUT);

  pinMode(LED_LEFT_PIN, OUTPUT);
  pinMode(LED_RIGHT_PIN, OUTPUT);

  digitalWrite(LED_LEFT_PIN, HIGH);
  digitalWrite(LED_RIGHT_PIN, HIGH);

  cmdLine.reserve(160);
}

void loop() {
  handleSerialCommands();

  unsigned long nowMicros = micros();
  if (nowMicros - lastSample < SAMPLE_PERIOD_US) return;
  lastSample += SAMPLE_PERIOD_US;

  unsigned long nowMs = millis();

  // ADC settle
  analogRead(HB100_LEFT_PIN);
  int rawLeft = analogRead(HB100_LEFT_PIN);

  analogRead(HB100_RIGHT_PIN);
  int rawRight = analogRead(HB100_RIGHT_PIN);

  int thresholdLeft = computeAdaptiveThreshold(noiseAvgAbsLeft, MIN_AMPLITUDE_LEFT, NOISE_MULT_LEFT, NOISE_OFFSET_LEFT);
  int thresholdRight = computeAdaptiveThreshold(noiseAvgAbsRight, MIN_AMPLITUDE_RIGHT, NOISE_MULT_RIGHT, NOISE_OFFSET_RIGHT);

  bool hb100DetectedMotionLeft = processHb(
    rawLeft,
    baselineLeft,
    noiseAvgAbsLeft,
    lastSignLeft,
    lastCrossLeft,
    expireLeft,
    eventsLeft,
    windowLeft,
    nowMicros,
    nowMs,
    thresholdLeft
  );

  bool hb100DetectedMotionRight = processHb(
    rawRight,
    baselineRight,
    noiseAvgAbsRight,
    lastSignRight,
    lastCrossRight,
    expireRight,
    eventsRight,
    windowRight,
    nowMicros,
    nowMs,
    thresholdRight
  );

  bool rcwlIsOverrideLeft  = rcwlOverride(RCWL_LEFT_PIN,  rcwlLeftStart,  nowMs, ENABLE_RCWL_LEFT);
  bool rcwlIsOverrideRight = rcwlOverride(RCWL_RIGHT_PIN, rcwlRightStart, nowMs, ENABLE_RCWL_RIGHT);

  int ledLeftOn = (hb100DetectedMotionLeft  && !rcwlIsOverrideLeft)  ? 1 : 0;
  int ledRightOn = (hb100DetectedMotionRight && !rcwlIsOverrideRight) ? 1 : 0;

  // RCWL ALWAYS override
  digitalWrite(LED_LEFT_PIN,  ledLeftOn ? LOW : HIGH);
  digitalWrite(LED_RIGHT_PIN, ledRightOn ? LOW : HIGH);

  // Telemetry to ESP32 every 100ms: leftVal, rightVal, rcwlLeftRaw, rcwlRightRaw, ledLeftOn, ledRightOn
  if (nowMs - lastTelemetryMs >= 100) {
    lastTelemetryMs = nowMs;

    int valLeft = rawLeft - (int)baselineLeft;
    int valRight = rawRight - (int)baselineRight;

    int rcwlLeftRaw = (digitalRead(RCWL_LEFT_PIN) == HIGH) ? 1 : 0;
    int rcwlRightRaw = (digitalRead(RCWL_RIGHT_PIN) == HIGH) ? 1 : 0;

    Serial.print("D,");
    Serial.print(valLeft);
    Serial.print(",");
    Serial.print(valRight);
    Serial.print(",");
    Serial.print(rcwlLeftRaw);
    Serial.print(",");
    Serial.print(rcwlRightRaw);
    Serial.print(",");
    Serial.print(ledLeftOn);
    Serial.print(",");
    Serial.println(ledRightOn);
  }
}
