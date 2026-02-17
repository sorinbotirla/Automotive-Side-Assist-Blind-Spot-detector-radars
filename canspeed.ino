// THIS file is used to send speed from CAN using an ESP32 LILYGO T-Display
// it connects to an ELM327 V1.5 
// (I used a Konnwei KW902 BT3.0 - any true V1.5 ELM327 will work, cheap clones might fail, classic bluetooth needed, no BLE for ELM)
// It sends speed to the other ESP32 using BLE.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "BluetoothSerial.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

TFT_eSPI tft = TFT_eSPI();
BluetoothSerial BT;

// ELM config
static const char *ELM_PIN = "1234";
// MAC from logs: 75:57:03:27:22:A4
static uint8_t ELM_ADDR[6] = { 0x75, 0x57, 0x03, 0x27, 0x22, 0xA4 }; // change it to your mac address of the ELM327 adadpter 


// BLE config
#define BLE_SERVICE_UUID    "3b07b6c0-7a4b-4c4e-9c7a-6d2d14a0e8f1"
#define BLE_CHAR_UUID       "3b07b6c1-7a4b-4c4e-9c7a-6d2d14a0e8f1"
static BLECharacteristic *speedChar = nullptr;
static bool bleClientConnected = false;
static uint32_t lastBleSendMs = 0;
static int lastBleSpeed = -1;
static const uint32_t BLE_SEND_MS = 200;

// Timing
static const uint32_t CONNECT_RETRY_MS = 2500;
static const uint32_t INIT_TIMEOUT_MS  = 12000;

static const uint32_t OBD_POLL_MS      = 400;
static const uint32_t OBD_TIMEOUT_MS   = 1200;

// ELM RX
static String rxBuf;

// Speed
static int vSpeed = -1;
static int lastDrawSpeed = -2;

enum State {
  ST_BT_CONNECT,
  ST_INIT_ATZ,
  ST_INIT_ATE0,
  ST_INIT_ATH0,
  ST_INIT_ATCAF0,
  ST_INIT_ATSP0,
  ST_INIT_ATDPN,
  ST_INIT_ATDP,
  ST_INIT_0100,
  ST_READY
};

static State st = ST_BT_CONNECT;
static uint32_t stMs = 0;

static uint32_t lastPollMs = 0;
static uint32_t pidMs = 0;
static bool waitingPid = false;

class BleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    bleClientConnected = true;
  }

  void onDisconnect(BLEServer *pServer) {
    bleClientConnected = false;
    pServer->getAdvertising()->start();
  }
};

static void bleInit()
{
  BLEDevice::init("TDisplaySpeed");

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new BleServerCallbacks());

  BLEService *service = server->createService(BLE_SERVICE_UUID);

  speedChar = service->createCharacteristic(
    BLE_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );

  speedChar->addDescriptor(new BLE2902());

  service->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
  adv->start();
}

static void bleSendSpeed(int speed)
{
  if (!bleClientConnected) return;
  if (!speedChar) return;
  if (speed < 0) return;

  uint32_t now = millis();

  bool changed = (speed != lastBleSpeed);
  bool periodic = (now - lastBleSendMs >= BLE_SEND_MS);

  if (!changed && !periodic) return;

  uint8_t payload[2];
  payload[0] = (uint8_t)(speed & 0xFF);
  payload[1] = (uint8_t)((speed >> 8) & 0xFF);

  speedChar->setValue(payload, 2);
  speedChar->notify();
  Serial.print("notify speed=");
  Serial.println(speed);

  lastBleSendMs = now;
  lastBleSpeed = speed;
}

static void clearRx()
{
  rxBuf = "";
}

static void pumpRx()
{
  while (BT.available()) {
    char c = (char)BT.read();
    rxBuf += c;
    if (rxBuf.length() > 1024) rxBuf.remove(0, rxBuf.length() - 1024);
  }
}

static bool hasPrompt()
{
  return rxBuf.indexOf('>') >= 0;
}

static void sendCmd(const char *cmd)
{
  BT.print(cmd);
  BT.print("\r");
}

static int hexNib(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  return -1;
}

static int parseHexByteAt(const String &s, int pos)
{
  if (pos + 1 >= (int)s.length()) return -1;
  int a = hexNib(s[pos]);
  int b = hexNib(s[pos + 1]);
  if (a < 0 || b < 0) return -1;
  return (a << 4) | b;
}

static void clearScreen()
{
  tft.fillScreen(TFT_BLACK);
}

static void drawCenterSmall(const String &s, int y, uint16_t color)
{
  tft.setTextFont(2);
  tft.setTextColor(color, TFT_BLACK);
  int x = (tft.width() - tft.textWidth(s)) / 2;
  if (x < 0) x = 0;
  tft.drawString(s, x, y, 2);
}

static void showStatus(const String &a, const String &b = "")
{
  clearScreen();
  drawCenterSmall(a, 44, TFT_WHITE);
  if (b.length()) drawCenterSmall(b, 68, TFT_WHITE);
}

static void drawSpeed()
{
  if (vSpeed == lastDrawSpeed) return;
  lastDrawSpeed = vSpeed;

  clearScreen();

  tft.setTextFont(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("Speed (km/h)", 10, 10, 2);

  tft.setTextFont(7);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  String v = (vSpeed >= 0) ? String(vSpeed) : String("-");
  int w = tft.textWidth(v);
  int x = (tft.width() - w) / 2;
  if (x < 0) x = 0;

  tft.drawString(v, x, 55, 7);
}

static void goState(State next)
{
  st = next;
  stMs = millis();
  clearRx();
}

static bool stateTimedOut(uint32_t ms)
{
  return (millis() - stMs) > ms;
}

static void btTick()
{
  pumpRx();

  if (!BT.connected()) {
    waitingPid = false;
    vSpeed = -1;
  }

  if (st == ST_BT_CONNECT) {
    if (!BT.connected()) {
      if (stateTimedOut(CONNECT_RETRY_MS)) {
        BT.disconnect();
        BT.connect(ELM_ADDR);
        stMs = millis();
        showStatus("Connecting", "ELM327");
      }
      return;
    }

    showStatus("Connected", "Init..");
    goState(ST_INIT_ATZ);
    sendCmd("ATZ");
    return;
  }

  if (!BT.connected()) {
    goState(ST_BT_CONNECT);
    return;
  }

  if (st == ST_INIT_ATZ) {
    if (hasPrompt()) {
      goState(ST_INIT_ATE0);
      sendCmd("ATE0");
      return;
    }
    if (stateTimedOut(INIT_TIMEOUT_MS)) { goState(ST_BT_CONNECT); return; }
    return;
  }

  if (st == ST_INIT_ATE0) {
    if (hasPrompt()) {
      goState(ST_INIT_ATH0);
      sendCmd("ATH0");
      return;
    }
    if (stateTimedOut(INIT_TIMEOUT_MS)) { goState(ST_BT_CONNECT); return; }
    return;
  }

  if (st == ST_INIT_ATH0) {
    if (hasPrompt()) {
      goState(ST_INIT_ATCAF0);
      sendCmd("ATCAF0");
      return;
    }
    if (stateTimedOut(INIT_TIMEOUT_MS)) { goState(ST_BT_CONNECT); return; }
    return;
  }

  if (st == ST_INIT_ATCAF0) {
    if (hasPrompt()) {
      goState(ST_INIT_ATSP0);
      sendCmd("ATSP0");
      return;
    }
    if (stateTimedOut(INIT_TIMEOUT_MS)) { goState(ST_BT_CONNECT); return; }
    return;
  }

  if (st == ST_INIT_ATSP0) {
    if (hasPrompt()) {
      goState(ST_INIT_ATDPN);
      sendCmd("ATDPN");
      return;
    }
    if (stateTimedOut(INIT_TIMEOUT_MS)) { goState(ST_BT_CONNECT); return; }
    return;
  }

  if (st == ST_INIT_ATDPN) {
    if (hasPrompt()) {
      goState(ST_INIT_ATDP);
      sendCmd("ATDP");
      return;
    }
    if (stateTimedOut(INIT_TIMEOUT_MS)) { goState(ST_BT_CONNECT); return; }
    return;
  }

  if (st == ST_INIT_ATDP) {
    if (hasPrompt()) {
      goState(ST_INIT_0100);
      sendCmd("0100");
      return;
    }
    if (stateTimedOut(INIT_TIMEOUT_MS)) { goState(ST_BT_CONNECT); return; }
    return;
  }

  if (st == ST_INIT_0100) {
    if (hasPrompt()) {
      goState(ST_READY);
      showStatus("Ready", "");
      lastPollMs = 0;
      waitingPid = false;
      return;
    }
    if (stateTimedOut(INIT_TIMEOUT_MS)) { goState(ST_BT_CONNECT); return; }
    return;
  }

  if (st == ST_READY) {
    uint32_t now = millis();

    if (!waitingPid && now - lastPollMs >= OBD_POLL_MS) {
      lastPollMs = now;
      clearRx();
      sendCmd("010D");
      pidMs = now;
      waitingPid = true;
      return;
    }

    if (waitingPid) {
      if (hasPrompt()) {
        String s = rxBuf;
        s.replace(" ", "");
        s.replace("\r", "");
        s.replace("\n", "");
        s.toUpperCase();

        int i = s.indexOf("410D");
        if (i >= 0 && i + 6 <= (int)s.length()) {
          int val = parseHexByteAt(s, i + 4);
          if (val >= 0) vSpeed = val;
        }

        waitingPid = false;
        return;
      }

      if (now - pidMs > OBD_TIMEOUT_MS) {
        waitingPid = false;
        return;
      }
    }

    return;
  }
}

void setup()
{
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);
  clearScreen();
  showStatus("Boot", "");

  BT.begin("TDisplay_OBD", true);
  BT.setPin(ELM_PIN, 4);

  bleInit();

  st = ST_BT_CONNECT;
  stMs = millis();
}

void loop()
{
  btTick();

  drawSpeed();
  bleSendSpeed(vSpeed);
}
