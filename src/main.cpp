#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <RTClib.h>
#include <ArduinoJson.h>
#include <math.h>
#include <esp_task_wdt.h>

// ---------------------------------------------------------------------------
// Pin map — ESP32 DOIT DevKit V1
// GPIO 34 is ADC1 (safe while WiFi is on; ADC2 is not). Input-only.
// GPIO 4 is not a strapping pin. GPIO 21/22 are the default I2C pins.
// ---------------------------------------------------------------------------
#define TDS_PIN 34
#define DS18B20_PIN 4
#define I2C_SDA 21
#define I2C_SCL 22

// DFRobot Gravity TDS Meter V1.0 (SEN0244):
// Analog output is 0–2.3 V. The cubic polynomial below is the vendor formula.
// UNO examples use analogRead() * 5.0 / 1024 (10-bit ADC, 5 V reference).
// This board is 12-bit with ADC_11db, full scale about 3.3 V, so the same
// step is analogRead() * 3.3 / 4096.
// The trailing 0.5 is the vendor default calibration coefficient (kValue).
static const float TDS_VREF = 3.3f;
static const float TDS_ADC_SCALE = 4096.0f;
static const float TDS_KVALUE = 0.5f;
static const float TDS_VMAX = 2.40f;
static const int TDS_SAMPLES = 30;
static const uint32_t DS18B20_CONV_MS = 800;

static const char *AP_SSID = "TDS-Monitor";
static const char *AP_PASSWORD = "12345678";

static const char *CONFIG_PATH = "/config.json";
static const char *DATA_PATH = "/data.csv";
static const char CSV_HEADER[] = "Date,Time,TDS (ppm),Temperature (\xC2\xB0" "C)\n";

static const uint32_t DEFAULT_MEASURE_MS = 10000;
static const uint32_t DEFAULT_LOG_MS = 60000;
static const uint32_t MIN_INTERVAL_MS = 1000;
static const uint32_t MAX_INTERVAL_MS = 86400000UL;
static const uint32_t WDT_TIMEOUT_S = 20;

WebServer server(80);
OneWire oneWire(DS18B20_PIN);
DallasTemperature sensors(&oneWire);
RTC_DS3231 rtc;

bool fsOk = false;
bool rtcOk = false;
bool rtcLostPower = false;
bool dsConnected = false;
bool tdsOk = false;
bool hasReading = false;
bool tempCompensated = false;

float currentTds = NAN;
float currentTemp = NAN;

uint32_t measurementInterval = DEFAULT_MEASURE_MS;
uint32_t logInterval = DEFAULT_LOG_MS;
uint32_t logCount = 0;

enum MeasureState { MEASURE_IDLE, MEASURE_WAIT_TEMP };
MeasureState measureState = MEASURE_IDLE;
unsigned long lastMeasureAt = 0;
unsigned long conversionStartedAt = 0;
unsigned long lastLogAt = 0;
unsigned long lastRtcProbeAt = 0;

bool intervalValid(uint32_t value) {
  return value >= MIN_INTERVAL_MS && value <= MAX_INTERVAL_MS;
}

void formatDateTime(char *dateOut, size_t dateLen, char *timeOut, size_t timeLen,
                    char *both, size_t bothLen) {
  if (!rtcOk) {
    if (dateOut && dateLen) dateOut[0] = '\0';
    if (timeOut && timeLen) timeOut[0] = '\0';
    snprintf(both, bothLen, "NOT FOUND");
    return;
  }
  DateTime now = rtc.now();
  snprintf(dateOut, dateLen, "%02d-%02d-%04d", now.day(), now.month(), now.year());
  snprintf(timeOut, timeLen, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  snprintf(both, bothLen, "%s %s", dateOut, timeOut);
}

String getDateTime() {
  char date[16], tim[16], both[32];
  formatDateTime(date, sizeof(date), tim, sizeof(tim), both, sizeof(both));
  return String(both);
}

void recoverI2C() {
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, OUTPUT);
  for (int i = 0; i < 16 && digitalRead(I2C_SDA) == LOW; i++) {
    digitalWrite(I2C_SCL, HIGH);
    delayMicroseconds(5);
    digitalWrite(I2C_SCL, LOW);
    delayMicroseconds(5);
  }
  pinMode(I2C_SDA, OUTPUT);
  digitalWrite(I2C_SDA, LOW);
  delayMicroseconds(5);
  digitalWrite(I2C_SCL, HIGH);
  delayMicroseconds(5);
  digitalWrite(I2C_SDA, HIGH);
  delayMicroseconds(5);
}

bool wireReady = false;
bool rtcScanPrinted = false;

bool initRTC() {
  if (!wireReady) {
    recoverI2C();
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000);
    wireReady = true;
  }
  Wire.setTimeOut(200);

  for (int attempt = 0; attempt < 3; attempt++) {
    if (rtc.begin()) {
      rtcOk = true;
      rtcLostPower = rtc.lostPower();
      return true;
    }
    delay(50);
  }

  rtcOk = false;
  if (!rtcScanPrinted) {
    rtcScanPrinted = true;
    Serial.println("RTC NOT FOUND on SDA 21 SCL 22. I2C scan:");
    Wire.setTimeOut(20);
    int found = 0;
    for (uint8_t address = 1; address < 127; address++) {
      Wire.beginTransmission(address);
      if (Wire.endTransmission() == 0) {
        Serial.printf("  device 0x%02X\n", address);
        found++;
      }
    }
    Wire.setTimeOut(200);
    if (found == 0) {
      Serial.println("  no I2C device. Check VCC 3.3V, GND, SDA=GPIO21, SCL=GPIO22.");
    }
  }
  return false;
}

bool setRTC(int year, int month, int day, int hour, int minute, int second) {
  if (!rtcOk) {
    return false;
  }
  if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31) {
    return false;
  }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
    return false;
  }
  rtc.adjust(DateTime(year, month, day, hour, minute, second));
  rtcLostPower = false;
  return true;
}

bool parseDate(const char *text, int &year, int &month, int &day) {
  int a = 0, b = 0, c = 0;
  if (sscanf(text, "%d-%d-%d", &a, &b, &c) != 3) {
    return false;
  }
  if (a > 31) {
    year = a;
    month = b;
    day = c;
  } else {
    day = a;
    month = b;
    year = c;
  }
  return year >= 2000 && year <= 2099 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

bool parseTime(const char *text, int &hour, int &minute, int &second) {
  second = 0;
  int n = sscanf(text, "%d:%d:%d", &hour, &minute, &second);
  if (n < 2) {
    return false;
  }
  return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59;
}

float readTemperature() {
  if (sensors.getDeviceCount() == 0) {
    dsConnected = false;
    return NAN;
  }
  dsConnected = true;
  float temp = sensors.getTempCByIndex(0);
  if (temp == DEVICE_DISCONNECTED_C || temp < -55.0f || temp > 125.0f) {
    dsConnected = false;
    return NAN;
  }
  return temp;
}

uint32_t medianMillis(uint32_t *values, int count) {
  for (int i = 1; i < count; i++) {
    uint32_t key = values[i];
    int j = i - 1;
    while (j >= 0 && values[j] > key) {
      values[j + 1] = values[j];
      j--;
    }
    values[j + 1] = key;
  }
  return values[count / 2];
}

// DFRobot SEN0244 temperature compensation, then the vendor cubic curve.
// temperature is °C from DS18B20. If it is NAN, 25 °C is used and the
// reading is marked as not temperature-compensated.
float readTDS(float temperature) {
  uint32_t samples[TDS_SAMPLES];
  for (int i = 0; i < TDS_SAMPLES; i++) {
    samples[i] = analogRead(TDS_PIN);
  }
  float voltage = medianMillis(samples, TDS_SAMPLES) * (TDS_VREF / TDS_ADC_SCALE);

  if (voltage < 0.0f || voltage > TDS_VMAX) {
    tdsOk = false;
    return NAN;
  }
  tdsOk = true;

  float tempC = temperature;
  if (isnan(tempC)) {
    tempC = 25.0f;
    tempCompensated = false;
  } else {
    tempCompensated = true;
  }

  float coefficient = 1.0f + 0.02f * (tempC - 25.0f);
  if (coefficient < 0.1f) {
    coefficient = 0.1f;
  }
  float compensated = voltage / coefficient;
  float tds = (133.42f * compensated * compensated * compensated
               - 255.86f * compensated * compensated
               + 857.39f * compensated) * TDS_KVALUE;
  if (tds < 0.0f) {
    tds = 0.0f;
  }
  return tds;
}

void ensureCsv() {
  if (LittleFS.exists(DATA_PATH)) {
    return;
  }
  File file = LittleFS.open(DATA_PATH, "w");
  if (!file) {
    return;
  }
  file.print(CSV_HEADER);
  file.close();
}

uint32_t countCsvRows() {
  File file = LittleFS.open(DATA_PATH, "r");
  if (!file) {
    return 0;
  }
  uint32_t lines = 0;
  while (file.available()) {
    if (file.read() == '\n') {
      lines++;
    }
  }
  file.close();
  return lines > 0 ? lines - 1 : 0;
}

bool recreateCsv() {
  if (LittleFS.exists(DATA_PATH)) {
    LittleFS.remove(DATA_PATH);
  }
  File file = LittleFS.open(DATA_PATH, "w");
  if (!file) {
    return false;
  }
  file.print(CSV_HEADER);
  file.close();
  logCount = 0;
  return true;
}

void loadConfig() {
  measurementInterval = DEFAULT_MEASURE_MS;
  logInterval = DEFAULT_LOG_MS;
  if (!LittleFS.exists(CONFIG_PATH)) {
    return;
  }
  File file = LittleFS.open(CONFIG_PATH, "r");
  if (!file) {
    return;
  }
  StaticJsonDocument<192> doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    return;
  }
  uint32_t measure = doc["measurementInterval"] | DEFAULT_MEASURE_MS;
  uint32_t logMs = doc["logInterval"] | DEFAULT_LOG_MS;
  if (intervalValid(measure)) {
    measurementInterval = measure;
  }
  if (intervalValid(logMs)) {
    logInterval = logMs;
  }
}

bool saveConfig() {
  File file = LittleFS.open(CONFIG_PATH, "w");
  if (!file) {
    return false;
  }
  StaticJsonDocument<128> doc;
  doc["measurementInterval"] = measurementInterval;
  doc["logInterval"] = logInterval;
  bool ok = serializeJson(doc, file) > 0;
  file.close();
  return ok;
}

void appendLog() {
  if (!fsOk || !rtcOk || !hasReading || isnan(currentTds)) {
    return;
  }
  char date[16], tim[16], both[32];
  formatDateTime(date, sizeof(date), tim, sizeof(tim), both, sizeof(both));

  File file = LittleFS.open(DATA_PATH, "a");
  if (!file) {
    return;
  }
  file.print(date);
  file.print(',');
  file.print(tim);
  file.print(',');
  file.print((int)lroundf(currentTds));
  file.print(',');
  if (!isnan(currentTemp)) {
    file.print(currentTemp, 1);
  }
  file.print('\n');
  file.close();
  logCount++;
}

void finishMeasurement() {
  float temp = NAN;
  if (dsConnected) {
    temp = readTemperature();
  }
  currentTemp = temp;
  currentTds = readTDS(temp);
  hasReading = true;
  Serial.printf("TDS %s ppm  Temp %s C  %s\n",
                isnan(currentTds) ? "--" : String(lroundf(currentTds)).c_str(),
                isnan(currentTemp) ? "--" : String(currentTemp, 1).c_str(),
                getDateTime().c_str());
}

void startMeasurement() {
  lastMeasureAt = millis();
  if (sensors.getDeviceCount() == 0) {
    dsConnected = false;
    finishMeasurement();
    measureState = MEASURE_IDLE;
    return;
  }
  dsConnected = true;
  sensors.requestTemperatures();
  conversionStartedAt = millis();
  measureState = MEASURE_WAIT_TEMP;
}

void serviceSensors() {
  unsigned long now = millis();
  if (measureState == MEASURE_WAIT_TEMP) {
    if (now - conversionStartedAt >= DS18B20_CONV_MS) {
      finishMeasurement();
      measureState = MEASURE_IDLE;
    }
    return;
  }
  if (!hasReading || (now - lastMeasureAt >= measurementInterval)) {
    startMeasurement();
  }
}

void serviceLog() {
  if (!hasReading) {
    return;
  }
  unsigned long now = millis();
  if (now - lastLogAt >= logInterval) {
    lastLogAt = now;
    appendLog();
  }
}

void sendJson(int code, const String &body) {
  server.send(code, "application/json", body);
}

void handleSensor() {
  StaticJsonDocument<192> doc;
  char both[32], date[16], tim[16];
  formatDateTime(date, sizeof(date), tim, sizeof(tim), both, sizeof(both));
  if (!hasReading || isnan(currentTds)) {
    doc["tds"] = nullptr;
  } else {
    doc["tds"] = (int)lroundf(currentTds);
  }
  if (!hasReading || isnan(currentTemp)) {
    doc["temperature"] = nullptr;
  } else {
    char tempBuf[12];
    dtostrf(currentTemp, 0, 1, tempBuf);
    doc["temperature"] = tempBuf;
  }
  doc["datetime"] = both;
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

void handleConfigGet() {
  StaticJsonDocument<128> doc;
  doc["measurementInterval"] = measurementInterval;
  doc["logInterval"] = logInterval;
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

void handleConfigPost() {
  StaticJsonDocument<192> doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    sendJson(400, "{\"ok\":false,\"error\":\"invalid json\"}");
    return;
  }
  uint32_t measure = doc["measurementInterval"] | 0;
  uint32_t logMs = doc["logInterval"] | 0;
  if (!intervalValid(measure) || !intervalValid(logMs)) {
    sendJson(400, "{\"ok\":false,\"error\":\"interval must be 1 to 86400 seconds\"}");
    return;
  }
  measurementInterval = measure;
  logInterval = logMs;
  if (!saveConfig()) {
    sendJson(500, "{\"ok\":false,\"error\":\"save failed\"}");
    return;
  }
  StaticJsonDocument<160> res;
  res["ok"] = true;
  res["measurementInterval"] = measurementInterval;
  res["logInterval"] = logInterval;
  String out;
  serializeJson(res, out);
  sendJson(200, out);
}

void handleRtcGet() {
  char date[16], tim[16], both[32];
  formatDateTime(date, sizeof(date), tim, sizeof(tim), both, sizeof(both));
  StaticJsonDocument<192> doc;
  doc["ok"] = rtcOk;
  doc["date"] = date;
  doc["time"] = tim;
  doc["datetime"] = both;
  doc["lostPower"] = rtcLostPower;
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

void handleRtcPost() {
  if (!rtcOk) {
    sendJson(503, "{\"ok\":false,\"error\":\"RTC NOT FOUND\"}");
    return;
  }
  StaticJsonDocument<192> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    sendJson(400, "{\"ok\":false,\"error\":\"invalid json\"}");
    return;
  }
  const char *dateText = doc["date"] | "";
  const char *timeText = doc["time"] | "";
  int year, month, day, hour, minute, second;
  if (!parseDate(dateText, year, month, day) || !parseTime(timeText, hour, minute, second)) {
    sendJson(400, "{\"ok\":false,\"error\":\"invalid date or time\"}");
    return;
  }
  if (!setRTC(year, month, day, hour, minute, second)) {
    sendJson(500, "{\"ok\":false,\"error\":\"set failed\"}");
    return;
  }
  StaticJsonDocument<128> res;
  res["ok"] = true;
  res["datetime"] = getDateTime();
  String out;
  serializeJson(res, out);
  sendJson(200, out);
}

void handleDataInfo() {
  size_t bytes = 0;
  if (LittleFS.exists(DATA_PATH)) {
    File file = LittleFS.open(DATA_PATH, "r");
    if (file) {
      bytes = file.size();
      file.close();
    }
  }
  StaticJsonDocument<256> doc;
  doc["count"] = logCount;
  doc["bytes"] = bytes;
  doc["littlefs"] = fsOk ? "ok" : "error";
  doc["totalBytes"] = fsOk ? LittleFS.totalBytes() : 0;
  doc["usedBytes"] = fsOk ? LittleFS.usedBytes() : 0;
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

void handleDownload() {
  File file = LittleFS.open(DATA_PATH, "r");
  if (!file) {
    sendJson(404, "{\"ok\":false,\"error\":\"file missing\"}");
    return;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=\"data.csv\"");
  server.streamFile(file, "text/csv");
  file.close();
}

void handleDelete() {
  if (!recreateCsv()) {
    sendJson(500, "{\"ok\":false,\"error\":\"delete failed\"}");
    return;
  }
  sendJson(200, "{\"ok\":true}");
}

void handleStatus() {
  StaticJsonDocument<384> doc;
  doc["littlefs"] = fsOk ? "ok" : "error";
  doc["rtc"] = rtcOk ? "connected" : "NOT FOUND";
  doc["ds18b20"] = dsConnected ? "connected" : "NOT FOUND";
  if (!hasReading) {
    doc["tds"] = "pending";
  } else {
    doc["tds"] = tdsOk ? "connected" : "error";
  }
  doc["wifi"] = "AP";
  doc["ssid"] = AP_SSID;
  doc["ip"] = WiFi.softAPIP().toString();
  doc["rtcLostPower"] = rtcLostPower;
  doc["tempCompensated"] = tempCompensated;
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

void handleRestart() {
  sendJson(200, "{\"ok\":true}");
  delay(250);
  ESP.restart();
}

void handleStatic(const char *path, const char *type) {
  File file = LittleFS.open(path, "r");
  if (!file) {
    server.send(404, "text/plain", "Not found. Upload LittleFS (pio run -t uploadfs).");
    return;
  }
  server.sendHeader("Cache-Control", "no-cache");
  server.streamFile(file, type);
  file.close();
}

void setupServer() {
  server.on("/api/sensor", HTTP_GET, handleSensor);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/rtc", HTTP_GET, handleRtcGet);
  server.on("/api/rtc", HTTP_POST, handleRtcPost);
  server.on("/api/data/info", HTTP_GET, handleDataInfo);
  server.on("/api/data/download", HTTP_GET, handleDownload);
  server.on("/api/data", HTTP_DELETE, handleDelete);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/restart", HTTP_POST, handleRestart);
  server.on("/", HTTP_GET, []() { handleStatic("/index.html", "text/html"); });
  server.on("/index.html", HTTP_GET, []() { handleStatic("/index.html", "text/html"); });
  server.on("/style.css", HTTP_GET, []() { handleStatic("/style.css", "text/css"); });
  server.on("/script.js", HTTP_GET, []() { handleStatic("/script.js", "application/javascript"); });
  server.begin();
}

void startWatchdog() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = WDT_TIMEOUT_S * 1000UL;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  esp_err_t cfgErr = esp_task_wdt_reconfigure(&cfg);
  if (cfgErr != ESP_OK) {
    cfgErr = esp_task_wdt_init(&cfg);
  }
#else
  esp_err_t cfgErr = esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_err_t addErr = esp_task_wdt_add(NULL);
  if (addErr == ESP_ERR_INVALID_ARG || addErr == ESP_ERR_INVALID_STATE) {
    addErr = ESP_OK;
  }
  if (cfgErr == ESP_OK && addErr == ESP_OK) {
    esp_task_wdt_reset();
    Serial.printf("Watchdog %u s\n", WDT_TIMEOUT_S);
  } else {
    Serial.printf("Watchdog setup failed cfg=%d add=%d\n", static_cast<int>(cfgErr), static_cast<int>(addErr));
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("TDS monitor");

  analogReadResolution(12);
  analogSetPinAttenuation(TDS_PIN, ADC_11db);

  fsOk = LittleFS.begin(false);
  if (!fsOk) {
    Serial.println("LittleFS mount failed, formatting");
    fsOk = LittleFS.begin(true);
  }
  if (fsOk) {
    loadConfig();
    if (!LittleFS.exists(CONFIG_PATH)) {
      saveConfig();
    }
    ensureCsv();
    logCount = countCsvRows();
  } else {
    Serial.println("LittleFS unavailable");
  }

  sensors.begin();
  sensors.setWaitForConversion(false);
  sensors.setResolution(12);
  dsConnected = sensors.getDeviceCount() > 0;

  if (!initRTC()) {
    Serial.println("RTC NOT FOUND");
  } else {
    Serial.println(getDateTime());
    if (rtcLostPower) {
      Serial.println("RTC lost power, set the clock from the web UI");
    }
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("AP ");
  Serial.print(AP_SSID);
  Serial.print("  http://");
  Serial.println(WiFi.softAPIP());

  setupServer();
  lastLogAt = millis();
  startMeasurement();
  startWatchdog();
}

void loop() {
  esp_task_wdt_reset();
  server.handleClient();
  if (!rtcOk && millis() - lastRtcProbeAt >= 5000) {
    lastRtcProbeAt = millis();
    if (initRTC()) {
      Serial.println(getDateTime());
    }
  }
  serviceSensors();
  serviceLog();
}
