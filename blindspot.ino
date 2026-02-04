// Dual HB100 + RCWL blind spot detector
// Timing / periods (ADJUSTABLE)
// Sample period in microseconds (200us = 5 kHz)
unsigned long SAMPLE_PERIOD_MICROSECONDS = 200;

// Doppler zero-crossing period limits in microseconds
unsigned long MIN_PERIOD_MICROSECONDS = 1000;
unsigned long MAX_PERIOD_MICROSECONDS = 500000;

// LED hold time after detection (milliseconds)
unsigned long MOTION_HOLD_MILISECONDS = 500;

// Event counting
int EVENTS_TO_TRIGGER = 1;
const unsigned long EVENT_WINDOW_MILISECONDS = 500;

// RCWL led OFF miliseconds:
unsigned long RCWL_MIN_ACTIVE_MILISECONDS = 1000;

// Pins
const int HB100_LEFT_PIN = A0;
const int HB100_RIGHT_PIN = A1;

const int RCWL_LEFT_PIN = 2;
const int RCWL_RIGHT_PIN = 3;

const bool RCWL_ACTIVE_LOW = false;

const int LED_LEFT_PIN = 10;
const int LED_RIGHT_PIN = 9;

bool ENABLE_RCWL_LEFT = true;
bool ENABLE_RCWL_RIGHT = true;

// Adaptive threshold settings
int MIN_AMPLITUDE_LEFT = 60;
int MIN_AMPLITUDE_RIGHT = 60;

int NOISE_MULT_LEFT = 0; // x10 units, 40 = 4.0x
int NOISE_MULT_RIGHT = 0;

int NOISE_OFFSET_LEFT = 0;
int NOISE_OFFSET_RIGHT = 0;

// noiseAvg += (abs(val)-noiseAvg) >> NOISE_AVERAGE_UPDATE_SPEED
int NOISE_AVERAGE_UPDATE_SPEED = 7;

// Runtime timing
unsigned long lastSampleMicroseconds = 0;
unsigned long lastTelemetryMiliseconds = 0;

// LEFT
long baselineLeft = 512;
bool lastSignLeft = false;
unsigned long lastCrossLeft = 0;
unsigned long expireLeft = 0;
int eventsLeft = 0;
unsigned long windowLeft  = 0;
unsigned long rcwlLeftStart = 0;
long noiseAvgAbsLeft = 0;
unsigned long rcwlLeftLastHigh = 0;

// RIGHT
long baselineRight = 512;
bool lastSignRight = false;
unsigned long lastCrossRight = 0;
unsigned long expireRight = 0;
int eventsRight = 0;
unsigned long windowRight = 0;
unsigned long rcwlRightStart = 0;
long noiseAvgAbsRight = 0;
unsigned long rcwlRightLastHigh = 0;

// Serial command parsing
static char cmdLine[180];
static uint8_t cmdLen = 0;

static int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static unsigned long clampULong(long v, unsigned long lo, unsigned long hi) {
  if (v < (long)lo) return lo;
  if (v > (long)hi) return hi;
  return (unsigned long)v;
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

// PWM fade + photoresistor settings
unsigned long FADE_IN_MILISECONDS = 300;
unsigned long FADE_OUT_MILISECONDS = 1500;

bool NIGHT_DIMMING_ENABLE = false;
int NIGHT_DIMMING_PERCENT = 70;

const int PHOTO_PIN = A6;
int PHOTO_DAY_TH = 650;
int PHOTO_NIGHT_TH = 550;
unsigned long PHOTO_CONFIRM_MILISECONDS = 1500;
int PHOTO_AVERAGE_UPDATE_SPEED = 4;

static void applySetting(const String &key, const String &val) {
  String k = key, v = val;
  k.trim();
  v.trim();

  // Timing / periods
  if (k == "SAMPLE_PERIOD_MICROSECONDS") {
    // keep sane: 50..5000us (20 kHz .. 200 Hz)
    SAMPLE_PERIOD_MICROSECONDS = clampULong(v.toInt(), 50, 5000);
    ackKV(k, String(SAMPLE_PERIOD_MICROSECONDS));
    return;
  }

  if (k == "MIN_PERIOD_MICROSECONDS") {
    // keep sane: 100..2000000us
    unsigned long newMin = clampULong(v.toInt(), 100, 2000000UL);
    // ensure min < max
    if (newMin >= MAX_PERIOD_MICROSECONDS) newMin = (MAX_PERIOD_MICROSECONDS > 101) ? (MAX_PERIOD_MICROSECONDS - 1) : 100;
    MIN_PERIOD_MICROSECONDS = newMin;
    ackKV(k, String(MIN_PERIOD_MICROSECONDS));
    return;
  }

  if (k == "MAX_PERIOD_MICROSECONDS") {
    // keep sane: 200..2000000us
    unsigned long newMax = clampULong(v.toInt(), 200, 2000000UL);
    // ensure max > min
    if (newMax <= MIN_PERIOD_MICROSECONDS) newMax = MIN_PERIOD_MICROSECONDS + 1;
    MAX_PERIOD_MICROSECONDS = newMax;
    ackKV(k, String(MAX_PERIOD_MICROSECONDS));
    return;
  }

  if (k == "MOTION_HOLD_MILISECONDS") {
    MOTION_HOLD_MILISECONDS = clampULong(v.toInt(), 0, 60000UL);
    ackKV(k, String(MOTION_HOLD_MILISECONDS));
    return;
  }

  // Event logic
  if (k == "EVENTS_TO_TRIGGER") {
    EVENTS_TO_TRIGGER = clampInt(v.toInt(), 1, 20);
    ackKV(k, String(EVENTS_TO_TRIGGER));
    return;
  }

  // RCWL
  if (k == "RCWL_MIN_ACTIVE_MILISECONDS") {
    RCWL_MIN_ACTIVE_MILISECONDS = clampULong(v.toInt(), 0, 60000UL);
    ackKV(k, String(RCWL_MIN_ACTIVE_MILISECONDS));
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

  // Adaptive threshold (per side)
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

  if (k == "NOISE_AVERAGE_UPDATE_SPEED") {
    NOISE_AVERAGE_UPDATE_SPEED = clampInt(v.toInt(), 4, 1000);
    ackKV(k, String(NOISE_AVERAGE_UPDATE_SPEED));
    return;
  }

  if (k == "FADE_IN_MILISECONDS") {
    FADE_IN_MILISECONDS = clampULong(v.toInt(), 0, 10000UL);
    ackKV(k, String(FADE_IN_MILISECONDS));
    return;
  }

  if (k == "FADE_OUT_MILISECONDS") {
    FADE_OUT_MILISECONDS = clampULong(v.toInt(), 0, 10000UL);
    ackKV(k, String(FADE_OUT_MILISECONDS));
    return;
  }

  if (k == "NIGHT_DIMMING_ENABLE") {
    NIGHT_DIMMING_ENABLE = strToBool(v);
    ackKV(k, NIGHT_DIMMING_ENABLE ? "1" : "0");
    return;
  }

  if (k == "NIGHT_DIMMING_PERCENT") {
    NIGHT_DIMMING_PERCENT = clampInt(v.toInt(), 1, 100);
    ackKV(k, String(NIGHT_DIMMING_PERCENT));
    return;
  }

  if (k == "PHOTO_DAY_TH") {
    PHOTO_DAY_TH = clampInt(v.toInt(), 0, 1023);
    ackKV(k, String(PHOTO_DAY_TH));
    return;
  }

  if (k == "PHOTO_NIGHT_TH") {
    PHOTO_NIGHT_TH = clampInt(v.toInt(), 0, 1023);
    ackKV(k, String(PHOTO_NIGHT_TH));
    return;
  }

  if (k == "PHOTO_CONFIRM_MILISECONDS") {
    PHOTO_CONFIRM_MILISECONDS = clampULong(v.toInt(), 0, 10000UL);
    ackKV(k, String(PHOTO_CONFIRM_MILISECONDS));
    return;
  }

  if (k == "PHOTO_AVERAGE_UPDATE_SPEED") {
    PHOTO_AVERAGE_UPDATE_SPEED = clampInt(v.toInt(), 1, 10);
    ackKV(k, String(PHOTO_AVERAGE_UPDATE_SPEED));
    return;
  }

  ackKV("UNKNOWN", k);
}

static void handleSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;

    if (c == '\n') {
      cmdLine[cmdLen] = 0;
      cmdLen = 0;

      char *s = cmdLine;
      while (*s == ' ' || *s == '\t') s++;
      if (!*s) continue;

      if (!(s[0] == 'C' && s[1] == ',')) continue;

      char *p1 = strchr(s + 2, ',');
      if (!p1) continue;

      *p1 = 0;
      String key = String(s + 2);
      String val = String(p1 + 1);
      applySetting(key, val);
      continue;
    }

    if (cmdLen < sizeof(cmdLine) - 1) {
      cmdLine[cmdLen++] = c;
    } else {
      cmdLen = 0;
    }
  }
}


// RCWL override
// tolerate brief LOW glitches while RCWL is "basically active"
unsigned long RCWL_GLITCH_TOLERANCE_MILISECONDS = 80;

bool rcwlOverride(
  int pin,
  unsigned long &startMs,
  unsigned long &lastHighMs,
  unsigned long nowMs,
  bool enabled
) {
  if (!enabled) {
    startMs = 0;
    lastHighMs = 0;
    return false;
  }

  bool high = (digitalRead(pin) == (RCWL_ACTIVE_LOW ? LOW : HIGH));

  if (high) {
    lastHighMs = nowMs;
    startMs = nowMs;
  } else {
    if (lastHighMs != 0 && (nowMs - lastHighMs) <= RCWL_GLITCH_TOLERANCE_MILISECONDS) {
      high = true;
    }
  }

  if (lastHighMs == 0) return false;
  return ((unsigned long)(nowMs - lastHighMs) <= RCWL_MIN_ACTIVE_MILISECONDS);
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

  int val = raw - (int)baseline;

  int a = val;
  if (a < 0) a = -a;

  if (a > noiseAvgAbs) {
    noiseAvgAbs += ((long)a - noiseAvgAbs) >> NOISE_AVERAGE_UPDATE_SPEED;
  } else {
    noiseAvgAbs -= (noiseAvgAbs - (long)a) >> NOISE_AVERAGE_UPDATE_SPEED;
  }

  bool sign = (val > 0);
  bool valid = false;

  if (!lastSign && sign && a > threshold) {
    if (lastCross != 0) {
      unsigned long period = nowMicros - lastCross;
      if (period > MIN_PERIOD_MICROSECONDS && period < MAX_PERIOD_MICROSECONDS) {
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
    } else if (nowMs - windowStart <= EVENT_WINDOW_MILISECONDS) {
      eventCount++;
    } else {
      windowStart = nowMs;
      eventCount = 1;
    }

    if (eventCount >= EVENTS_TO_TRIGGER) {
      expireMs = nowMs + MOTION_HOLD_MILISECONDS;
      eventCount = 0;
    }
  } else if (eventCount && nowMs - windowStart > EVENT_WINDOW_MILISECONDS) {
    eventCount = 0;
  }

  return (nowMs < expireMs);
}

class LedFader {
public:
  void begin(uint8_t pwmPin, bool invertedPwm, bool useGamma) {
    _pin = pwmPin;
    _inverted = invertedPwm;
    _gamma = useGamma;
    pinMode(_pin, OUTPUT);
    _cur = 0;
    _mode = IDLE;
    writeStableOrPwm(0);
  }

  void fadeTo(uint8_t target, unsigned long durationMs) {
    unsigned long now = millis();
    uint8_t currentNow = current();

    _startB = currentNow;
    _endB = target;
    _t0 = now;

    if (durationMs == 0) {
      _dur = 1;
      _mode = IDLE;
      _cur = _endB;
      writeStableOrPwm(_cur);
      return;
    }

    _dur = durationMs;
    _mode = FADE;

    _cur = currentNow;
    writeStableOrPwm(_cur);
  }

  void update() {
    if (_mode == IDLE) return;

    unsigned long now = millis();
    unsigned long dt = (unsigned long)(now - _t0);

    if (dt >= _dur) {
      _cur = _endB;
      _mode = IDLE;
      writeStableOrPwm(_cur);
      return;
    }

    uint8_t b = interpolate(now, _t0, _dur, _startB, _endB);
    _cur = b;
    writeStableOrPwm(_cur);
  }

  uint8_t current() const {
    if (_mode == IDLE) return _cur;
    unsigned long now = millis();
    return interpolate(now, _t0, _dur, _startB, _endB);
  }

  bool isOffStable() const { return (_mode == IDLE && _cur == 0); }
  bool isOnStable() const { return (_mode == IDLE && _cur == 255); }
  bool isIdle() const { return (_mode == IDLE); }

private:
  enum Mode : uint8_t { IDLE, FADE };

  uint8_t _pin = 255;
  bool _inverted = false;
  bool _gamma = true;

  Mode _mode = IDLE;

  uint8_t _cur = 0;
  uint8_t _startB = 0;
  uint8_t _endB = 0;

  unsigned long _t0 = 0;
  unsigned long _dur = 1;

  static uint8_t gamma8(uint8_t x) {
    return (uint16_t(x) * uint16_t(x) + 255) >> 8;
  }

  static uint8_t interpolate(unsigned long now, unsigned long t0, unsigned long dur, uint8_t a, uint8_t b) {
    unsigned long dt = (unsigned long)(now - t0);
    if (dt >= dur) return b;

    int16_t diff = (int16_t)b - (int16_t)a;
    int16_t val = (int16_t)a + (int32_t)diff * (int32_t)dt / (int32_t)dur;

    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (uint8_t)val;
  }

  void writeStableOrPwm(uint8_t logicalBrightness) const {
    uint8_t out = _gamma ? gamma8(logicalBrightness) : logicalBrightness;
    if (_inverted) out = 255 - out;

    if (_mode == IDLE && (logicalBrightness == 0 || logicalBrightness == 255)) {
      digitalWrite(_pin, (out >= 128) ? HIGH : LOW);
      return;
    }

    analogWrite(_pin, out);
  }
};

LedFader ledLeft;
LedFader ledRight;

static int photoAvg = 0;
static bool isNight = false;
static unsigned long photoCandidateStart = 0;

static int iabs(int v) { return (v < 0) ? -v : v; }

static void photoInit() {
  pinMode(PHOTO_PIN, INPUT);
  int r = analogRead(PHOTO_PIN);
  photoAvg = (r < 0) ? 0 : r;
  isNight = false;
  photoCandidateStart = 0;
}

static void photoUpdate(unsigned long nowMs) {
  int raw = analogRead(PHOTO_PIN);
  if (raw < 0) raw = 0;
  if (raw > 1023) raw = 1023;

  int shift = PHOTO_AVERAGE_UPDATE_SPEED;
  if (shift < 1) shift = 1;
  if (shift > 10) shift = 10;

  photoAvg += (raw - photoAvg) >> shift;

  if (!NIGHT_DIMMING_ENABLE) {
    isNight = false;
    photoCandidateStart = 0;
    return;
  }

  bool wantNight = isNight;

  if (!isNight) {
    if (photoAvg < PHOTO_NIGHT_TH) wantNight = true;
  } else {
    if (photoAvg > PHOTO_DAY_TH) wantNight = false;
  }

  if (wantNight == isNight) {
    photoCandidateStart = 0;
    return;
  }

  if (photoCandidateStart == 0) photoCandidateStart = nowMs;
  if ((unsigned long)(nowMs - photoCandidateStart) >= PHOTO_CONFIRM_MILISECONDS) {
    isNight = wantNight;
    photoCandidateStart = 0;
  }
}

static uint8_t currentMaxBrightness() {
  int pct = NIGHT_DIMMING_PERCENT;
  if (pct < 1) pct = 1;
  if (pct > 100) pct = 100;

  if (!NIGHT_DIMMING_ENABLE) return 255;
  if (!isNight) return 255;

  return (uint8_t)((255L * (long)pct) / 100L);
}

static void applyLedTargets(bool wantOnLeft, bool wantOnRight, uint8_t cap) {
  static bool lastWantOnLeft = false;
  static bool lastWantOnRight = false;
  static uint8_t lastCap = 255;

  if (wantOnLeft != lastWantOnLeft) {
    if (wantOnLeft) ledLeft.fadeTo(cap, FADE_IN_MILISECONDS);
    else ledLeft.fadeTo(0, FADE_OUT_MILISECONDS);
    lastWantOnLeft = wantOnLeft;
  } else if (wantOnLeft && cap != lastCap && ledLeft.isIdle()) {
    ledLeft.fadeTo(cap, 120);
  }

  if (wantOnRight != lastWantOnRight) {
    if (wantOnRight) ledRight.fadeTo(cap, FADE_IN_MILISECONDS);
    else ledRight.fadeTo(0, FADE_OUT_MILISECONDS);
    lastWantOnRight = wantOnRight;
  } else if (wantOnRight && cap != lastCap && ledRight.isIdle()) {
    ledRight.fadeTo(cap, 120);
  }

  lastCap = cap;
}

void setup() {
  Serial.begin(115200);

  analogReference(DEFAULT);

  pinMode(HB100_LEFT_PIN, INPUT);
  pinMode(HB100_RIGHT_PIN, INPUT);

  pinMode(RCWL_LEFT_PIN, INPUT_PULLUP);
  pinMode(RCWL_RIGHT_PIN, INPUT_PULLUP);
  

  ledLeft.begin(LED_LEFT_PIN, false, true);
  ledRight.begin(LED_RIGHT_PIN, false, true);

  photoInit();

  // start aligned, prevent "catch-up bursts"
  lastSampleMicroseconds = micros();
}

void loop() {
  handleSerialCommands();

  ledLeft.update();
  ledRight.update();

  unsigned long nowMicros = micros();
  if (nowMicros - lastSampleMicroseconds < SAMPLE_PERIOD_MICROSECONDS) return;

  // drop missed samples (do NOT catch up)
  lastSampleMicroseconds = nowMicros;

  unsigned long nowMs = millis();

  photoUpdate(nowMs);

  // ADC settle
  analogRead(HB100_LEFT_PIN);
  int rawLeft = analogRead(HB100_LEFT_PIN);

  analogRead(HB100_RIGHT_PIN);
  int rawRight = analogRead(HB100_RIGHT_PIN);

  int thresholdLeft  = computeAdaptiveThreshold(noiseAvgAbsLeft,  MIN_AMPLITUDE_LEFT,  NOISE_MULT_LEFT,  NOISE_OFFSET_LEFT);
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

  bool rcwlIsOverrideLeft  = rcwlOverride(RCWL_LEFT_PIN,  rcwlLeftStart,  rcwlLeftLastHigh,  nowMs, ENABLE_RCWL_LEFT);
  bool rcwlIsOverrideRight = rcwlOverride(RCWL_RIGHT_PIN, rcwlRightStart, rcwlRightLastHigh, nowMs, ENABLE_RCWL_RIGHT);

  int ledLeftOn  = (hb100DetectedMotionLeft  && !rcwlIsOverrideLeft)  ? 1 : 0;
  int ledRightOn = (hb100DetectedMotionRight && !rcwlIsOverrideRight) ? 1 : 0;

  uint8_t cap = currentMaxBrightness();
  applyLedTargets(ledLeftOn == 1, ledRightOn == 1, cap);

  // Telemetry to ESP32 every 100ms:
  // D,valLeft,valRight,rcwlLeftRaw,rcwlRightRaw,ledLeftOn,ledRightOn,rcwlOverrideLeft,rcwlOverrideRight
  if (nowMs - lastTelemetryMiliseconds >= 100) {
    lastTelemetryMiliseconds = nowMs;

    int valLeft  = rawLeft  - (int)baselineLeft;
    int valRight = rawRight - (int)baselineRight;

    int rcwlLeftRaw  = (digitalRead(RCWL_LEFT_PIN)  == (RCWL_ACTIVE_LOW ? LOW : HIGH)) ? 1 : 0;
    int rcwlRightRaw = (digitalRead(RCWL_RIGHT_PIN) == (RCWL_ACTIVE_LOW ? LOW : HIGH)) ? 1 : 0;

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
    Serial.print(ledRightOn);
    Serial.print(",");
    Serial.print(rcwlIsOverrideLeft ? 1 : 0);
    Serial.print(",");
    Serial.println(rcwlIsOverrideRight ? 1 : 0);
  }
}
