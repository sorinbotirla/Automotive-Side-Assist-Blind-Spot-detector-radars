#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>
#include <NimBLEDevice.h>

static const int UART_RX = 20;
static const int UART_TX = 21;

HardwareSerial Link(0);

static const int SD_CS = 10;
static const int SD_SCK = 6;
static const int SD_MOSI = 7;
static const int SD_MISO = 5;

SPIClass sdSPI(FSPI);
WebServer server(80);

static bool httpStarted = false;
static bool sdMissingPrinted = false;
static bool sdOk = false;

static bool loggingEnabled = false;
static String currentLogName = "";
static unsigned long logStartMs = 0;
static unsigned long flushLastMs = 0;

static const int BATCH_LINES = 100;
static const int LINE_BYTES = 110;
static const int OUTBUF_MAX = BATCH_LINES * LINE_BYTES;

static char outbuf[OUTBUF_MAX];
static int outlen = 0;
static int outLines = 0;

static String uartLine = "";

static String wifiSsid = "";
static String wifiPass = "";

static const char *SETTINGS_PATH = "/settings/radarsettings.json";
static const char *LOG_DIR = "/logs";

// Legacy (old UI)
static int settings_MIN_AMPLITUDE = 60;

// Timing / periods (new)
static unsigned long settings_SAMPLE_PERIOD_MICROSECONDS = 200;
static unsigned long settings_MIN_PERIOD_MICROSECONDS = 1000;
static unsigned long settings_MAX_PERIOD_MICROSECONDS = 500000;

// Motion hold
static unsigned long settings_MOTION_HOLD_MILISECONDS = 500;

static unsigned long settings_FADE_IN_MILISECONDS = 600;
static unsigned long settings_FADE_OUT_MILISECONDS = 1500;

// Event counting
static int settings_EVENTS_TO_TRIGGER = 1;

// RCWL override
static unsigned long settings_RCWL_MIN_ACTIVE_MILISECONDS = 1000;
static bool settings_ENABLE_RCWL_LEFT = true;
static bool settings_ENABLE_RCWL_RIGHT = true;

// Adaptive threshold (per side)
static int settings_MIN_AMPLITUDE_LEFT = 60;
static int settings_MIN_AMPLITUDE_RIGHT = 60;

static int settings_NOISE_MULT_LEFT = 0;
static int settings_NOISE_MULT_RIGHT = 0;

static int settings_NOISE_OFFSET_LEFT = 0;
static int settings_NOISE_OFFSET_RIGHT = 0;

static int settings_NOISE_AVERAGE_UPDATE_SPEED = 7;

// Speed gate
static int settings_MIN_ACTIVE_SPEED = 0;

static String lastAckLine = "";

// Live HB100 averages (about 1s)
static long liveSumL = 0;
static long liveSumR = 0;
static long liveSumAbsL = 0;
static long liveSumAbsR = 0;
static unsigned long liveCnt = 0;

static unsigned long liveWindowStartMs = 0;
static int liveAvgL = 0;
static int liveAvgR = 0;
static int liveAbsAvgL = 0;
static int liveAbsAvgR = 0;

static int liveLastL = 0;
static int liveLastR = 0;

static int liveRcwlLeft = 0;
static int liveRcwlRight = 0;
static int liveOvLeft = 0;
static int liveOvRight = 0;

static unsigned long liveLastSampleMs = 0;

// Live car speed (from BLE)
static int liveCarSpeed = 0;
static unsigned long liveCarSpeedMs = 0;

static int iabs(int v) { return (v < 0) ? -v : v; }

static void liveFeedSample(int l, int r, int rl, int rr, unsigned long nowMs) {
  liveLastL = l;
  liveLastR = r;
  liveLastSampleMs = nowMs;

  liveRcwlLeft = (rl ? 1 : 0);
  liveRcwlRight = (rr ? 1 : 0);

  if (liveWindowStartMs == 0) liveWindowStartMs = nowMs;

  liveSumL += l;
  liveSumR += r;
  liveSumAbsL += iabs(l);
  liveSumAbsR += iabs(r);
  liveCnt++;

  if (nowMs - liveWindowStartMs >= 1000) {
    if (liveCnt > 0) {
      liveAvgL = (int)(liveSumL / (long)liveCnt);
      liveAvgR = (int)(liveSumR / (long)liveCnt);
      liveAbsAvgL = (int)(liveSumAbsL / (long)liveCnt);
      liveAbsAvgR = (int)(liveSumAbsR / (long)liveCnt);
    }

    liveSumL = 0;
    liveSumR = 0;
    liveSumAbsL = 0;
    liveSumAbsR = 0;
    liveCnt = 0;
    liveWindowStartMs = nowMs;
  }
}

static String trimLine(String s) {
  s.replace("\r", "");
  s.trim();
  return s;
}

static String joinPath(const String &a, const String &b) {
  if (a.endsWith("/")) return a + b;
  return a + "/" + b;
}

static bool ensureDir(const char *path) {
  if (!sdOk) return false;
  if (SD.exists(path)) return true;
  return SD.mkdir(path);
}

static bool readWifiCredentialsFromSD() {
  wifiSsid = "";
  wifiPass = "";

  File f = SD.open("/wifi/wificredentials.txt", FILE_READ);
  if (!f) return false;

  String s1 = f.readStringUntil('\n');
  String s2 = f.readStringUntil('\n');
  f.close();

  s1 = trimLine(s1);
  s2 = trimLine(s2);

  if (s1.length() == 0) return false;

  wifiSsid = s1;
  wifiPass = s2;
  return true;
}

static bool wifiStartStaOnly() {
  Serial.println("wifi: reading /wifi/wificredentials.txt");

  if (!sdOk) {
    Serial.println("sd: missing");
    return false;
  }

  if (!readWifiCredentialsFromSD()) {
    Serial.println("wifi: missing or invalid credentials file");
    return false;
  }

  Serial.print("wifi: ssid=");
  Serial.println(wifiSsid);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  Serial.println("wifi: connecting");
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("wifi: connected");
    Serial.print("wifi: ip=");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("wifi: connect failed");
  return false;
}

static String contentTypeFromPath(const String &path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".jpg")) return "image/jpeg";
  if (path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".gif")) return "image/gif";
  if (path.endsWith(".svg")) return "image/svg+xml";
  if (path.endsWith(".ico")) return "image/x-icon";
  if (path.endsWith(".txt")) return "text/plain";
  if (path.endsWith(".json")) return "application/json";
  return "application/octet-stream";
}

static bool serveFileFromSD(const String &sdPath) {
  if (!sdOk) return false;
  if (!SD.exists(sdPath.c_str())) return false;

  File f = SD.open(sdPath.c_str(), FILE_READ);
  if (!f) return false;

  String ctype = contentTypeFromPath(sdPath);
  server.streamFile(f, ctype);
  f.close();
  return true;
}

static void flushBatchToSD() {
  if (!loggingEnabled) return;
  if (!sdOk) return;
  if (currentLogName.length() == 0) return;
  if (outlen <= 0) return;

  String path = joinPath(String(LOG_DIR), currentLogName);

  File f = SD.open(path.c_str(), FILE_APPEND);
  if (!f) {
    outlen = 0;
    outLines = 0;
    return;
  }

  f.write((const uint8_t*)outbuf, outlen);
  f.close();

  outlen = 0;
  outLines = 0;
}

static int findNextLogIndex() {
  int maxIdx = 0;

  if (!sdOk) return 1;

  File root = SD.open(LOG_DIR);
  if (!root) return 1;

  File entry = root.openNextFile();
  while (entry) {
    String name = String(entry.name());
    entry.close();

    name.toLowerCase();

    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);

    if (name.startsWith("radarlog") && name.endsWith(".log")) {
      String mid = name.substring(String("radarlog").length(), name.length() - String(".log").length());
      int n = mid.toInt();
      if (n > maxIdx) maxIdx = n;
    }

    entry = root.openNextFile();
  }

  root.close();
  return maxIdx + 1;
}

static void startLogging() {
  if (!sdOk) {
    Serial.println("sd: missing");
    return;
  }

  ensureDir(LOG_DIR);
  flushBatchToSD();

  int idx = findNextLogIndex();
  currentLogName = "radarlog" + String(idx) + ".log";

  String path = joinPath(String(LOG_DIR), currentLogName);

  File f = SD.open(path.c_str(), FILE_WRITE);
  if (f) {
    f.close();
    loggingEnabled = true;
    logStartMs = millis();
    flushLastMs = millis();
    Serial.print("logging started: ");
    Serial.println(path);
  } else {
    loggingEnabled = false;
    currentLogName = "";
  }
}

static void stopLogging() {
  loggingEnabled = false;
  flushBatchToSD();
  Serial.println("logging stopped");
}

static String jsonEscape(const String &s) {
  String out = "";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

static String settingsToJson() {
  settings_MIN_AMPLITUDE = settings_MIN_AMPLITUDE_LEFT;

  String body = "{";

  body += "\"MIN_AMPLITUDE\":"; body += String(settings_MIN_AMPLITUDE); body += ",";
  body += "\"MOTION_HOLD_MS\":"; body += String(settings_MOTION_HOLD_MILISECONDS); body += ",";
  body += "\"RCWL_MIN_ACTIVE_MS\":"; body += String(settings_RCWL_MIN_ACTIVE_MILISECONDS); body += ",";
  body += "\"NOISE_ALPHA_SHIFT\":"; body += String(settings_NOISE_AVERAGE_UPDATE_SPEED); body += ",";

  body += "\"SAMPLE_PERIOD_MICROSECONDS\":"; body += String(settings_SAMPLE_PERIOD_MICROSECONDS); body += ",";
  body += "\"MIN_PERIOD_MICROSECONDS\":"; body += String(settings_MIN_PERIOD_MICROSECONDS); body += ",";
  body += "\"MAX_PERIOD_MICROSECONDS\":"; body += String(settings_MAX_PERIOD_MICROSECONDS); body += ",";

  body += "\"MOTION_HOLD_MILISECONDS\":"; body += String(settings_MOTION_HOLD_MILISECONDS); body += ",";
  body += "\"FADE_IN_MILISECONDS\":"; body += String(settings_FADE_IN_MILISECONDS); body += ",";
  body += "\"FADE_OUT_MILISECONDS\":"; body += String(settings_FADE_OUT_MILISECONDS); body += ",";
  body += "\"EVENTS_TO_TRIGGER\":"; body += String(settings_EVENTS_TO_TRIGGER); body += ",";
  body += "\"RCWL_MIN_ACTIVE_MILISECONDS\":"; body += String(settings_RCWL_MIN_ACTIVE_MILISECONDS); body += ",";

  body += "\"ENABLE_RCWL_LEFT\":"; body += (settings_ENABLE_RCWL_LEFT ? "true" : "false"); body += ",";
  body += "\"ENABLE_RCWL_RIGHT\":"; body += (settings_ENABLE_RCWL_RIGHT ? "true" : "false"); body += ",";

  body += "\"MIN_AMPLITUDE_LEFT\":"; body += String(settings_MIN_AMPLITUDE_LEFT); body += ",";
  body += "\"MIN_AMPLITUDE_RIGHT\":"; body += String(settings_MIN_AMPLITUDE_RIGHT); body += ",";

  body += "\"NOISE_MULT_LEFT\":"; body += String(settings_NOISE_MULT_LEFT); body += ",";
  body += "\"NOISE_MULT_RIGHT\":"; body += String(settings_NOISE_MULT_RIGHT); body += ",";

  body += "\"NOISE_OFFSET_LEFT\":"; body += String(settings_NOISE_OFFSET_LEFT); body += ",";
  body += "\"NOISE_OFFSET_RIGHT\":"; body += String(settings_NOISE_OFFSET_RIGHT); body += ",";

  body += "\"NOISE_AVERAGE_UPDATE_SPEED\":"; body += String(settings_NOISE_AVERAGE_UPDATE_SPEED); body += ",";

  body += "\"MIN_ACTIVE_SPEED\":"; body += String(settings_MIN_ACTIVE_SPEED);

  body += "}";
  return body;
}

static int jsonFindKeyPos(const String &json, const char *key) {
  String needle = "\"";
  needle += key;
  needle += "\"";
  return json.indexOf(needle);
}

static bool jsonReadInt(const String &json, const char *key, long &outVal) {
  int kp = jsonFindKeyPos(json, key);
  if (kp < 0) return false;

  int colon = json.indexOf(':', kp);
  if (colon < 0) return false;

  int i = colon + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) i++;

  bool neg = false;
  if (i < (int)json.length() && json[i] == '-') { neg = true; i++; }

  long v = 0;
  bool any = false;

  while (i < (int)json.length()) {
    char c = json[i];
    if (c >= '0' && c <= '9') {
      any = true;
      v = (v * 10) + (c - '0');
      i++;
    } else {
      break;
    }
  }

  if (!any) return false;
  if (neg) v = -v;
  outVal = v;
  return true;
}

static bool jsonReadBool(const String &json, const char *key, bool &outVal) {
  int kp = jsonFindKeyPos(json, key);
  if (kp < 0) return false;

  int colon = json.indexOf(':', kp);
  if (colon < 0) return false;

  int i = colon + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) i++;

  if (json.startsWith("true", i)) { outVal = true; return true; }
  if (json.startsWith("false", i)) { outVal = false; return true; }

  if (i < (int)json.length() && json[i] == '1') { outVal = true; return true; }
  if (i < (int)json.length() && json[i] == '0') { outVal = false; return true; }

  return false;
}

static bool readSettingsFromSD() {
  if (!sdOk) return false;
  if (!SD.exists(SETTINGS_PATH)) return false;

  File f = SD.open(SETTINGS_PATH, FILE_READ);
  if (!f) return false;

  String json = "";
  json.reserve(1024);

  while (f.available()) {
    json += (char)f.read();
    if (json.length() > 4096) break;
  }
  f.close();

  json = trimLine(json);
  if (json.length() == 0) return false;

  long li = 0;
  bool lb = false;

  if (jsonReadInt(json, "MIN_AMPLITUDE", li)) {
    settings_MIN_AMPLITUDE = (int)li;
    settings_MIN_AMPLITUDE_LEFT = settings_MIN_AMPLITUDE;
    settings_MIN_AMPLITUDE_RIGHT = settings_MIN_AMPLITUDE;
  }

  if (jsonReadInt(json, "SAMPLE_PERIOD_MICROSECONDS", li)) settings_SAMPLE_PERIOD_MICROSECONDS = (unsigned long)li;
  if (jsonReadInt(json, "MIN_PERIOD_MICROSECONDS", li)) settings_MIN_PERIOD_MICROSECONDS = (unsigned long)li;
  if (jsonReadInt(json, "MAX_PERIOD_MICROSECONDS", li)) settings_MAX_PERIOD_MICROSECONDS = (unsigned long)li;

  if (jsonReadInt(json, "SAMPLE_PERIOD_US", li)) settings_SAMPLE_PERIOD_MICROSECONDS = (unsigned long)li;
  if (jsonReadInt(json, "MIN_PERIOD_US", li)) settings_MIN_PERIOD_MICROSECONDS = (unsigned long)li;
  if (jsonReadInt(json, "MAX_PERIOD_US", li)) settings_MAX_PERIOD_MICROSECONDS = (unsigned long)li;

  if (jsonReadInt(json, "MOTION_HOLD_MILISECONDS", li)) settings_MOTION_HOLD_MILISECONDS = (unsigned long)li;
  if (jsonReadInt(json, "MOTION_HOLD_MS", li)) settings_MOTION_HOLD_MILISECONDS = (unsigned long)li;

  if (jsonReadInt(json, "FADE_IN_MILISECONDS", li)) settings_FADE_IN_MILISECONDS = (unsigned long)li;
  if (jsonReadInt(json, "FADE_OUT_MILISECONDS", li)) settings_FADE_OUT_MILISECONDS = (unsigned long)li;

  if (jsonReadInt(json, "RCWL_MIN_ACTIVE_MILISECONDS", li)) settings_RCWL_MIN_ACTIVE_MILISECONDS = (unsigned long)li;
  if (jsonReadInt(json, "RCWL_MIN_ACTIVE_MS", li)) settings_RCWL_MIN_ACTIVE_MILISECONDS = (unsigned long)li;

  if (jsonReadInt(json, "EVENTS_TO_TRIGGER", li)) settings_EVENTS_TO_TRIGGER = (int)li;

  if (jsonReadBool(json, "ENABLE_RCWL_LEFT", lb)) settings_ENABLE_RCWL_LEFT = lb;
  if (jsonReadBool(json, "ENABLE_RCWL_RIGHT", lb)) settings_ENABLE_RCWL_RIGHT = lb;

  if (jsonReadInt(json, "MIN_AMPLITUDE_LEFT", li)) settings_MIN_AMPLITUDE_LEFT = (int)li;
  if (jsonReadInt(json, "MIN_AMPLITUDE_RIGHT", li)) settings_MIN_AMPLITUDE_RIGHT = (int)li;

  if (jsonReadInt(json, "NOISE_MULT_LEFT", li)) settings_NOISE_MULT_LEFT = (int)li;
  if (jsonReadInt(json, "NOISE_MULT_RIGHT", li)) settings_NOISE_MULT_RIGHT = (int)li;

  if (jsonReadInt(json, "NOISE_OFFSET_LEFT", li)) settings_NOISE_OFFSET_LEFT = (int)li;
  if (jsonReadInt(json, "NOISE_OFFSET_RIGHT", li)) settings_NOISE_OFFSET_RIGHT = (int)li;

  if (jsonReadInt(json, "NOISE_AVERAGE_UPDATE_SPEED", li)) settings_NOISE_AVERAGE_UPDATE_SPEED = (int)li;
  if (jsonReadInt(json, "NOISE_ALPHA_SHIFT", li)) settings_NOISE_AVERAGE_UPDATE_SPEED = (int)li;

  if (jsonReadInt(json, "MIN_ACTIVE_SPEED", li)) settings_MIN_ACTIVE_SPEED = (int)li;

  return true;
}

static bool writeSettingsToSD() {
  if (!sdOk) return false;

  SD.mkdir("/settings");

  File f = SD.open(SETTINGS_PATH, FILE_WRITE);
  if (!f) return false;

  String json = settingsToJson();
  f.print(json);
  f.close();
  return true;
}

static void sendCmdToArduino(const String &key, const String &val) {
  Link.print("C,");
  Link.print(key);
  Link.print(",");
  Link.println(val);
}

static void pushAllSettingsToArduino() {
  sendCmdToArduino("SAMPLE_PERIOD_MICROSECONDS", String(settings_SAMPLE_PERIOD_MICROSECONDS));
  sendCmdToArduino("MIN_PERIOD_MICROSECONDS", String(settings_MIN_PERIOD_MICROSECONDS));
  sendCmdToArduino("MAX_PERIOD_MICROSECONDS", String(settings_MAX_PERIOD_MICROSECONDS));

  sendCmdToArduino("MOTION_HOLD_MILISECONDS", String(settings_MOTION_HOLD_MILISECONDS));

  sendCmdToArduino("FADE_IN_MILISECONDS", String(settings_FADE_IN_MILISECONDS));
  sendCmdToArduino("FADE_OUT_MILISECONDS", String(settings_FADE_OUT_MILISECONDS));

  sendCmdToArduino("EVENTS_TO_TRIGGER", String(settings_EVENTS_TO_TRIGGER));

  sendCmdToArduino("RCWL_MIN_ACTIVE_MILISECONDS", String(settings_RCWL_MIN_ACTIVE_MILISECONDS));
  sendCmdToArduino("ENABLE_RCWL_LEFT", settings_ENABLE_RCWL_LEFT ? "1" : "0");
  sendCmdToArduino("ENABLE_RCWL_RIGHT", settings_ENABLE_RCWL_RIGHT ? "1" : "0");

  sendCmdToArduino("MIN_AMPLITUDE_LEFT", String(settings_MIN_AMPLITUDE_LEFT));
  sendCmdToArduino("MIN_AMPLITUDE_RIGHT", String(settings_MIN_AMPLITUDE_RIGHT));

  sendCmdToArduino("NOISE_MULT_LEFT", String(settings_NOISE_MULT_LEFT));
  sendCmdToArduino("NOISE_MULT_RIGHT", String(settings_NOISE_MULT_RIGHT));

  sendCmdToArduino("NOISE_OFFSET_LEFT", String(settings_NOISE_OFFSET_LEFT));
  sendCmdToArduino("NOISE_OFFSET_RIGHT", String(settings_NOISE_OFFSET_RIGHT));

  sendCmdToArduino("NOISE_AVERAGE_UPDATE_SPEED", String(settings_NOISE_AVERAGE_UPDATE_SPEED));

  sendCmdToArduino("MIN_ACTIVE_SPEED", String(settings_MIN_ACTIVE_SPEED));
}

static void handleApiLive() {
  unsigned long nowMs = millis();
  unsigned long age = (liveLastSampleMs == 0) ? 0 : (nowMs - liveLastSampleMs);
  unsigned long speedAge = (liveCarSpeedMs == 0) ? 0 : (nowMs - liveCarSpeedMs);

  String body = "{";
  body += "\"ok\":true,";

  body += "\"hb_left_avg\":"; body += String(liveAvgL); body += ",";
  body += "\"hb_right_avg\":"; body += String(liveAvgR); body += ",";
  body += "\"hb_left_absavg\":"; body += String(liveAbsAvgL); body += ",";
  body += "\"hb_right_absavg\":"; body += String(liveAbsAvgR); body += ",";
  body += "\"hb_left_last\":"; body += String(liveLastL); body += ",";
  body += "\"hb_right_last\":"; body += String(liveLastR); body += ",";

  body += "\"rcwl_left\":"; body += String(liveRcwlLeft); body += ",";
  body += "\"rcwl_right\":"; body += String(liveRcwlRight); body += ",";
  body += "\"ov_left\":"; body += String(liveOvLeft); body += ",";
  body += "\"ov_right\":"; body += String(liveOvRight); body += ",";

  body += "\"speed\":"; body += String(liveCarSpeed); body += ",";
  body += "\"speed_age_ms\":"; body += String(speedAge); body += ",";

  body += "\"age_ms\":"; body += String(age);
  body += "}";

  server.send(200, "application/json", body);
}

static void handleApiStatus() {
  String body = "{";
  body += "\"logging\":"; body += (loggingEnabled ? "true" : "false"); body += ",";
  body += "\"file\":\""; body += jsonEscape(currentLogName); body += "\"";
  body += "}";
  server.send(200, "application/json", body);
}

static void handleApiList() {
  String body = "{";
  body += "\"files\":[";

  bool first = true;

  if (sdOk) {
    File root = SD.open(LOG_DIR);
    if (root) {
      File entry = root.openNextFile();
      while (entry) {
        String name = String(entry.name());
        entry.close();

        name.toLowerCase();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);

        if (name.startsWith("radarlog") && name.endsWith(".log")) {
          if (!first) body += ",";
          first = false;
          body += "\"";
          body += jsonEscape(name);
          body += "\"";
        }

        entry = root.openNextFile();
      }
      root.close();
    }
  }

  body += "]}";
  server.send(200, "application/json", body);
}

static void handleApiStart() {
  startLogging();
  handleApiStatus();
}

static void handleApiStop() {
  stopLogging();
  handleApiStatus();
}

static void handleApiChunk() {
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "missing name");
    return;
  }

  if (!sdOk) {
    server.send(503, "text/plain", "sd missing");
    return;
  }

  String name = server.arg("name");
  name.toLowerCase();

  if (!name.startsWith("radarlog") || !name.endsWith(".log")) {
    server.send(400, "text/plain", "bad name");
    return;
  }

  long offset = 0;
  long limit = 1000;

  if (server.hasArg("offset")) offset = server.arg("offset").toInt();
  if (server.hasArg("limit")) limit = server.arg("limit").toInt();

  if (offset < 0) offset = 0;
  if (limit < 1) limit = 1;
  if (limit > 2000) limit = 2000;

  String path = joinPath(String(LOG_DIR), name);

  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) {
    server.send(404, "text/plain", "not found");
    return;
  }

  String out = "";
  out.reserve(1024);

  long lineNo = 0;
  long emitted = 0;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.replace("\r", "");
    line.trim();

    if (lineNo >= offset) {
      out += line;
      out += "\n";
      emitted++;
      if (emitted >= limit) break;
    }

    lineNo++;
  }

  f.close();
  server.send(200, "text/plain", out);
}

static void handleApiDelete() {
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "missing name");
    return;
  }

  if (!sdOk) {
    server.send(503, "text/plain", "sd missing");
    return;
  }

  String name = server.arg("name");
  name.toLowerCase();

  if (!name.startsWith("radarlog") || !name.endsWith(".log")) {
    server.send(400, "text/plain", "bad name");
    return;
  }

  if (loggingEnabled && currentLogName.length() > 0) {
    String cur = currentLogName;
    cur.toLowerCase();
    if (cur == name) {
      server.send(409, "text/plain", "cannot delete active log");
      return;
    }
  }

  String path = joinPath(String(LOG_DIR), name);

  if (!SD.exists(path.c_str())) {
    server.send(404, "text/plain", "not found");
    return;
  }

  if (!SD.remove(path.c_str())) {
    server.send(500, "text/plain", "delete failed");
    return;
  }

  server.send(200, "text/plain", "ok");
}

static void handleApiSettingsGet() {
  String json = settingsToJson();
  server.send(200, "application/json", json);
}

static void handleApiSettingsReload() {
  bool ok = readSettingsFromSD();
  if (ok) {
    pushAllSettingsToArduino();
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    server.send(404, "application/json", "{\"ok\":false}");
  }
}

static void sendCanonicalSettingToArduino(const String &key, const String &val) {
  if (key == "MOTION_HOLD_MS") { sendCmdToArduino("MOTION_HOLD_MILISECONDS", String(settings_MOTION_HOLD_MILISECONDS)); return; }
  if (key == "RCWL_MIN_ACTIVE_MS") { sendCmdToArduino("RCWL_MIN_ACTIVE_MILISECONDS", String(settings_RCWL_MIN_ACTIVE_MILISECONDS)); return; }
  if (key == "NOISE_ALPHA_SHIFT") { sendCmdToArduino("NOISE_AVERAGE_UPDATE_SPEED", String(settings_NOISE_AVERAGE_UPDATE_SPEED)); return; }
  if (key == "SAMPLE_PERIOD_US") { sendCmdToArduino("SAMPLE_PERIOD_MICROSECONDS", String(settings_SAMPLE_PERIOD_MICROSECONDS)); return; }
  if (key == "MIN_PERIOD_US") { sendCmdToArduino("MIN_PERIOD_MICROSECONDS", String(settings_MIN_PERIOD_MICROSECONDS)); return; }
  if (key == "MAX_PERIOD_US") { sendCmdToArduino("MAX_PERIOD_MICROSECONDS", String(settings_MAX_PERIOD_MICROSECONDS)); return; }

  sendCmdToArduino(key, val);
}

static bool applySettingKeyValue(const String &key, const String &val) {
  String k = key, v = val, x = "";
  long li = 0;
  k.trim();
  v.trim();

  if (k == "MIN_AMPLITUDE") {
    li = v.toInt();
    settings_MIN_AMPLITUDE = (int)li;
    settings_MIN_AMPLITUDE_LEFT = settings_MIN_AMPLITUDE;
    settings_MIN_AMPLITUDE_RIGHT = settings_MIN_AMPLITUDE;
    return true;
  }

  if (k == "SAMPLE_PERIOD_MICROSECONDS" || k == "SAMPLE_PERIOD_US") { settings_SAMPLE_PERIOD_MICROSECONDS = (unsigned long)v.toInt(); return true; }
  if (k == "MIN_PERIOD_MICROSECONDS" || k == "MIN_PERIOD_US") { settings_MIN_PERIOD_MICROSECONDS = (unsigned long)v.toInt(); return true; }
  if (k == "MAX_PERIOD_MICROSECONDS" || k == "MAX_PERIOD_US") { settings_MAX_PERIOD_MICROSECONDS = (unsigned long)v.toInt(); return true; }

  if (k == "MOTION_HOLD_MILISECONDS" || k == "MOTION_HOLD_MS") { settings_MOTION_HOLD_MILISECONDS = (unsigned long)v.toInt(); return true; }

  if (k == "FADE_IN_MILISECONDS") { settings_FADE_IN_MILISECONDS = (unsigned long)v.toInt(); return true; }
  if (k == "FADE_OUT_MILISECONDS") { settings_FADE_OUT_MILISECONDS = (unsigned long)v.toInt(); return true; }

  if (k == "EVENTS_TO_TRIGGER") { settings_EVENTS_TO_TRIGGER = v.toInt(); return true; }

  if (k == "RCWL_MIN_ACTIVE_MILISECONDS" || k == "RCWL_MIN_ACTIVE_MS") { settings_RCWL_MIN_ACTIVE_MILISECONDS = (unsigned long)v.toInt(); return true; }

  if (k == "ENABLE_RCWL_LEFT") { x = v; x.toLowerCase(); settings_ENABLE_RCWL_LEFT = (x == "1" || x == "true" || x == "on" || x == "yes"); return true; }
  if (k == "ENABLE_RCWL_RIGHT") { x = v; x.toLowerCase(); settings_ENABLE_RCWL_RIGHT = (x == "1" || x == "true" || x == "on" || x == "yes"); return true; }

  if (k == "MIN_AMPLITUDE_LEFT") { settings_MIN_AMPLITUDE_LEFT = v.toInt(); settings_MIN_AMPLITUDE = settings_MIN_AMPLITUDE_LEFT; return true; }
  if (k == "MIN_AMPLITUDE_RIGHT") { settings_MIN_AMPLITUDE_RIGHT = v.toInt(); return true; }

  if (k == "NOISE_MULT_LEFT") { settings_NOISE_MULT_LEFT = v.toInt(); return true; }
  if (k == "NOISE_MULT_RIGHT") { settings_NOISE_MULT_RIGHT = v.toInt(); return true; }

  if (k == "NOISE_OFFSET_LEFT") { settings_NOISE_OFFSET_LEFT = v.toInt(); return true; }
  if (k == "NOISE_OFFSET_RIGHT") { settings_NOISE_OFFSET_RIGHT = v.toInt(); return true; }

  if (k == "NOISE_AVERAGE_UPDATE_SPEED" || k == "NOISE_ALPHA_SHIFT") { settings_NOISE_AVERAGE_UPDATE_SPEED = v.toInt(); return true; }

  if (k == "MIN_ACTIVE_SPEED") { settings_MIN_ACTIVE_SPEED = v.toInt(); return true; }

  return false;
}

static void handleApiSettingsSet() {
  if (!server.hasArg("key") || !server.hasArg("value")) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }

  String key = server.arg("key");
  String val = server.arg("value");

  if (!applySettingKeyValue(key, val)) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }

  if (key == "MIN_AMPLITUDE") {
    sendCmdToArduino("MIN_AMPLITUDE_LEFT", String(settings_MIN_AMPLITUDE_LEFT));
    sendCmdToArduino("MIN_AMPLITUDE_RIGHT", String(settings_MIN_AMPLITUDE_RIGHT));
  } else if (key == "MIN_ACTIVE_SPEED") {
    sendCmdToArduino("MIN_ACTIVE_SPEED", String(settings_MIN_ACTIVE_SPEED));
  } else {
    sendCanonicalSettingToArduino(key, val);
  }

  String body = "{\"ok\":true,\"ack\":\"";
  body += jsonEscape(lastAckLine);
  body += "\"}";
  server.send(200, "application/json", body);
}

static void handleApiSettingsSave() {
  bool ok = writeSettingsToSD();
  if (ok) server.send(200, "application/json", "{\"ok\":true}");
  else server.send(500, "application/json", "{\"ok\":false}");
}

static void setupHttp() {
  server.on("/", []() {
    if (!serveFileFromSD("/webapp/index.html")) server.send(404, "text/plain", "missing /webapp/index.html");
  });

  server.on("/assets/js/jquery.js", []() {
    if (!serveFileFromSD("/webapp/assets/js/jquery.js")) server.send(404, "text/plain", "missing jquery.js");
  });

  server.on("/assets/js/dygraph-min.js", []() {
    if (!serveFileFromSD("/webapp/assets/js/dygraph-min.js")) server.send(404, "text/plain", "missing dygraph-min.js");
  });

  server.on("/assets/js/dygraph-extra.js", []() {
    if (!serveFileFromSD("/webapp/assets/js/dygraph-extra.js")) server.send(404, "text/plain", "missing dygraph-extra.js");
  });

  server.on("/assets/js/app.js", []() {
    if (!serveFileFromSD("/webapp/assets/js/app.js")) server.send(404, "text/plain", "missing app.js");
  });

  server.on("/assets/css/style.css", []() {
    if (!serveFileFromSD("/webapp/assets/css/style.css")) server.send(404, "text/plain", "missing style.css");
  });

  server.on("/api/status", handleApiStatus);
  server.on("/api/list", handleApiList);
  server.on("/api/start", handleApiStart);
  server.on("/api/stop", handleApiStop);
  server.on("/api/chunk", handleApiChunk);
  server.on("/api/delete", handleApiDelete);

  server.on("/api/settings/get", handleApiSettingsGet);
  server.on("/api/settings/reload", handleApiSettingsReload);
  server.on("/api/settings/set", handleApiSettingsSet);
  server.on("/api/settings/save", handleApiSettingsSave);

  server.on("/api/live", handleApiLive);

  server.onNotFound([]() {
    String uri = server.uri();
    String sdPath = "/webapp" + uri;
    if (!serveFileFromSD(sdPath)) server.send(404, "text/plain", "not found");
  });

  server.begin();
}

// BLE speed receiver: client + notify subscribe
static const char *BLE_SPEED_SVC_UUID = "3b07b6c0-7a4b-4c4e-9c7a-6d2d14a0e8f1";
static const char *BLE_SPEED_CHR_UUID = "3b07b6c1-7a4b-4c4e-9c7a-6d2d14a0e8f1";

static NimBLEAdvertisedDevice *bleFound = nullptr;
static NimBLEClient *bleCli = nullptr;
static NimBLERemoteCharacteristic *bleSpeedChr = nullptr;

static unsigned long bleLastActionMs = 0;

static void applyCarSpeed(int spd) {
  if (spd < 0) spd = 0;
  if (spd > 300) spd = 300;

  liveCarSpeed = spd;
  liveCarSpeedMs = millis();

  sendCmdToArduino("CAR_SPEED", String(liveCarSpeed));
}

static void bleOnNotify(NimBLERemoteCharacteristic *c, uint8_t *data, size_t len, bool isNotify) {
  if (len < 2) return;

  uint16_t u = (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
  applyCarSpeed((int)u);

  Serial.print("ble speed=");
  Serial.println(liveCarSpeed);
}

class BleScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice *d) {
    if (!d) return;
    if (!d->haveServiceUUID()) return;
    if (!d->isAdvertisingService(NimBLEUUID(BLE_SPEED_SVC_UUID))) return;

    bleFound = d;
    NimBLEDevice::getScan()->stop();
  }
};

class BleClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *pClient) {
    Serial.println("ble: connected");
  }

  void onDisconnect(NimBLEClient *pClient) {
    Serial.println("ble: disconnected");
    bleSpeedChr = nullptr;
    bleFound = nullptr;
  }
};

static void bleStartScan() {
  bleFound = nullptr;

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new BleScanCallbacks(), true);
  scan->setActiveScan(true);
  scan->start(3, false);

  Serial.println("ble: scanning");
}

static bool bleConnectAndSubscribe() {
  if (!bleFound) return false;

  if (!bleCli) {
    bleCli = NimBLEDevice::createClient();
    bleCli->setClientCallbacks(new BleClientCallbacks(), false);
  }

  if (!bleCli->connect(bleFound)) {
    Serial.println("ble: connect failed");
    return false;
  }

  NimBLERemoteService *svc = bleCli->getService(BLE_SPEED_SVC_UUID);
  if (!svc) {
    Serial.println("ble: service missing");
    bleCli->disconnect();
    return false;
  }

  bleSpeedChr = svc->getCharacteristic(BLE_SPEED_CHR_UUID);
  if (!bleSpeedChr) {
    Serial.println("ble: char missing");
    bleCli->disconnect();
    return false;
  }

  if (!bleSpeedChr->canNotify()) {
    Serial.println("ble: notify not supported");
    bleCli->disconnect();
    return false;
  }

  if (!bleSpeedChr->subscribe(true, bleOnNotify)) {
    Serial.println("ble: subscribe failed");
    bleCli->disconnect();
    bleSpeedChr = nullptr;
    return false;
  }

  Serial.println("ble: subscribed");
  return true;
}

static void bleInit() {
  NimBLEDevice::init("BlindspotC3");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  bleStartScan();
  bleLastActionMs = millis();
}

static void bleTick() {
  unsigned long now = millis();

  if (bleCli && bleCli->isConnected() && bleSpeedChr) return;

  if ((unsigned long)(now - bleLastActionMs) < 2000) return;
  bleLastActionMs = now;

  if (!bleFound) {
    bleStartScan();
    return;
  }

  bleConnectAndSubscribe();
}

// Telemetry parser: accepts 4, 6, or 8 ints.
// 4:  l,r,rl,rr
// 6:  l,r,rl,rr,ledL,ledR
// 8:  l,r,rl,rr,ledL,ledR,ovL,ovR
static bool parseCsv4or6or8(
  const String &sIn,
  int &l, int &r, int &rl, int &rr,
  int &ledL, int &ledR,
  int &ovL, int &ovR
) {
  String s = trimLine(sIn);
  if (s.length() == 0) return false;

  int vals[8], idx = 0, start = 0;

  for (int i = 0; i <= (int)s.length(); i++) {
    char ch = (i == (int)s.length()) ? ',' : s[i];
    if (ch == ',') {
      if (idx >= 8) return false;
      String part = s.substring(start, i);
      part.trim();
      vals[idx] = part.toInt();
      idx++;
      start = i + 1;
    }
  }

  if (idx != 4 && idx != 6 && idx != 8) return false;

  l = vals[0];
  r = vals[1];
  rl = vals[2];
  rr = vals[3];

  ledL = 0;
  ledR = 0;
  ovL = 0;
  ovR = 0;

  if (idx >= 6) {
    ledL = vals[4];
    ledR = vals[5];
  }
  if (idx == 8) {
    ovL = vals[6];
    ovR = vals[7];
  }

  return true;
}

static bool parseDataLine(
  const String &line,
  int &l, int &r, int &rl, int &rr,
  int &ledL, int &ledR,
  int &ovL, int &ovR
) {
  String s = trimLine(line);
  if (s.length() == 0) return false;

  if (s.startsWith("D,")) {
    String rest = trimLine(s.substring(2));
    return parseCsv4or6or8(rest, l, r, rl, rr, ledL, ledR, ovL, ovR);
  }

  return parseCsv4or6or8(s, l, r, rl, rr, ledL, ledR, ovL, ovR);
}

static void handleUartLine(const String &line, unsigned long nowMs) {
  String s = trimLine(line);
  if (s.length() == 0) return;

  if (s.startsWith("A,")) {
    lastAckLine = s;
    return;
  }

  int l = 0, r = 0, rl = 0, rr = 0, ledL = 0, ledR = 0, ovL = 0, ovR = 0;
  if (!parseDataLine(s, l, r, rl, rr, ledL, ledR, ovL, ovR)) return;

  liveFeedSample(l, r, rl, rr, nowMs);

  liveRcwlLeft = (rl ? 1 : 0);
  liveRcwlRight = (rr ? 1 : 0);
  liveOvLeft = (ovL ? 1 : 0);
  liveOvRight = (ovR ? 1 : 0);

  if (loggingEnabled && sdOk) {
    unsigned long ts = nowMs - logStartMs;

    int n = snprintf(outbuf + outlen, OUTBUF_MAX - outlen,
                     "%d, %d, %d, %d, %d, %d, %d, %d, %lu\n",
                     l, r, rl, rr, ledL, ledR, ovL, ovR, (unsigned long)ts);

    if (n > 0 && (outlen + n) < OUTBUF_MAX) {
      outlen += n;
      outLines++;
      if (outLines >= BATCH_LINES) flushBatchToSD();
    } else {
      flushBatchToSD();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  Link.begin(115200, SERIAL_8N1, UART_RX, UART_TX);

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, sdSPI);

  if (!sdOk) {
    Serial.println("sd: missing");
    httpStarted = false;
    return;
  }

  ensureDir(LOG_DIR);
  SD.mkdir("/settings");

  if (readSettingsFromSD()) pushAllSettingsToArduino();
  else pushAllSettingsToArduino();

  wifiStartStaOnly();

  setupHttp();
  httpStarted = true;

  bleInit();

  uartLine.reserve(200);
  lastAckLine.reserve(160);
}

void loop() {
  if (!sdOk) {
    if (!sdMissingPrinted) {
      sdMissingPrinted = true;
      Serial.println("sd: missing");
    }
    delay(500);
    return;
  }

  if (httpStarted) {
    server.handleClient();
  }

  bleTick();

  unsigned long nowMs = millis();

  while (Link.available()) {
    char c = (char)Link.read();
    if (c == '\r') continue;

    if (c == '\n') {
      if (uartLine.length() > 0) {
        handleUartLine(uartLine, nowMs);
      }
      uartLine = "";
    } else {
      if (uartLine.length() < 180) uartLine += c;
    }
  }

  if (loggingEnabled && sdOk) {
    if (nowMs - flushLastMs >= 2000) {
      flushLastMs = nowMs;
      if (outlen > 0) flushBatchToSD();
    }
  }
}
