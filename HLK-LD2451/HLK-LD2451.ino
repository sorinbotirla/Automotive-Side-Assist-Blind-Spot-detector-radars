#include <Arduino.h>

static const int REVERSE_PIN = 2;

static const int LEFT_RADAR_ALERT_PIN = 4;
static const int RIGHT_RADAR_ALERT_PIN = 3;

static const int LEFT_SIGNAL_PIN = 5;
static const int RIGHT_SIGNAL_PIN = 6;

static const int LEFT_RELAY_PIN = 8;
static const int RIGHT_RELAY_PIN = 9;

static const uint32_t STARTUP_TEST_MS = 2000;
static const uint32_t REVERSE_RELEASE_LOCKOUT_MS = 5000;
static const uint32_t SIGNAL_HOLD_MS = 1200;
static const uint32_t FAST_BLINK_MS = 150;

static uint32_t bootMs = 0;
static uint32_t reverseReleasedMs = 0;

static uint32_t leftLastSignalMs = 0;
static uint32_t rightLastSignalMs = 0;

static uint32_t leftBlinkMs = 0;
static uint32_t rightBlinkMs = 0;

static bool startupDone = false;
static bool previousReverseActive = false;

static bool leftBlinkState = false;
static bool rightBlinkState = false;

void leftRelayOn() {
  digitalWrite(LEFT_RELAY_PIN, LOW);
}

void leftRelayOff() {
  digitalWrite(LEFT_RELAY_PIN, HIGH);
}

void rightRelayOn() {
  digitalWrite(RIGHT_RELAY_PIN, LOW);
}

void rightRelayOff() {
  digitalWrite(RIGHT_RELAY_PIN, HIGH);
}

void bothRelaysOn() {
  leftRelayOn();
  rightRelayOn();
}

void bothRelaysOff() {
  leftRelayOff();
  rightRelayOff();
}

void updateBlink(uint32_t now, uint32_t &lastBlinkMs, bool &blinkState) {
  if ((now - lastBlinkMs) >= FAST_BLINK_MS) {
    lastBlinkMs = now;
    blinkState = !blinkState;
  }
}

void setup() {
  pinMode(LEFT_RELAY_PIN, OUTPUT);
  pinMode(RIGHT_RELAY_PIN, OUTPUT);

  bothRelaysOff();

  pinMode(REVERSE_PIN, INPUT);
  pinMode(LEFT_RADAR_ALERT_PIN, INPUT);
  pinMode(RIGHT_RADAR_ALERT_PIN, INPUT);
  pinMode(LEFT_SIGNAL_PIN, INPUT);
  pinMode(RIGHT_SIGNAL_PIN, INPUT);

  bootMs = millis();
  leftBlinkMs = bootMs;
  rightBlinkMs = bootMs;
}

void loop() {
  uint32_t now = millis();

  if (!startupDone) {
    if ((now - bootMs) < STARTUP_TEST_MS) {
      bothRelaysOn();
      return;
    }

    startupDone = true;
    bothRelaysOff();
    return;
  }

  bool reverseNow = digitalRead(REVERSE_PIN) == HIGH;

  if (reverseNow) {
    previousReverseActive = true;
    bothRelaysOff();
    return;
  }

  if (previousReverseActive && !reverseNow) {
    previousReverseActive = false;
    reverseReleasedMs = now;
    bothRelaysOff();
    return;
  }

  if ((now - reverseReleasedMs) < REVERSE_RELEASE_LOCKOUT_MS) {
    bothRelaysOff();
    return;
  }

  bool leftRadarActive = digitalRead(LEFT_RADAR_ALERT_PIN) == HIGH;
  bool rightRadarActive = digitalRead(RIGHT_RADAR_ALERT_PIN) == HIGH;

  if (digitalRead(LEFT_SIGNAL_PIN) == HIGH) {
    leftLastSignalMs = now;
  }

  if (digitalRead(RIGHT_SIGNAL_PIN) == HIGH) {
    rightLastSignalMs = now;
  }

  bool leftSignalHeld = (now - leftLastSignalMs) <= SIGNAL_HOLD_MS;
  bool rightSignalHeld = (now - rightLastSignalMs) <= SIGNAL_HOLD_MS;

  if (leftRadarActive && leftSignalHeld) {
    updateBlink(now, leftBlinkMs, leftBlinkState);

    if (leftBlinkState) {
      leftRelayOn();
    } else {
      leftRelayOff();
    }
  } else if (leftRadarActive) {
    leftRelayOn();
    leftBlinkState = false;
    leftBlinkMs = now;
  } else {
    leftRelayOff();
    leftBlinkState = false;
    leftBlinkMs = now;
  }

  if (rightRadarActive && rightSignalHeld) {
    updateBlink(now, rightBlinkMs, rightBlinkState);

    if (rightBlinkState) {
      rightRelayOn();
    } else {
      rightRelayOff();
    }
  } else if (rightRadarActive) {
    rightRelayOn();
    rightBlinkState = false;
    rightBlinkMs = now;
  } else {
    rightRelayOff();
    rightBlinkState = false;
    rightBlinkMs = now;
  }
}
