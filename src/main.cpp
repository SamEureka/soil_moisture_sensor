#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <BH1750.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"

// --- Credential guards ---
#ifndef WIFI_SSID
#define WIFI_SSID "fallback_ssid"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "fallback_pass"
#endif
#ifndef AIO_USERNAME
#define AIO_USERNAME "fallback_user"
#endif
#ifndef AIO_KEY
#define AIO_KEY "fallback_key"
#endif
#ifndef AIO_SERVER
#define AIO_SERVER "io.adafruit.com"
#endif
#ifndef AIO_SERVERPORT
#define AIO_SERVERPORT 1883
#endif

// --- Pin guards ---
#ifndef MOISTURE_PIN
#define MOISTURE_PIN D0
#endif
#ifndef SENSOR_POWER_PIN
#define SENSOR_POWER_PIN D2
#endif

// --- Calibration guards ---
#ifndef ADC_SAMPLES
#define ADC_SAMPLES 10
#endif
#ifndef ADC_SAMPLE_DELAY_MS
#define ADC_SAMPLE_DELAY_MS 5
#endif
#ifndef AIR_VALUE
#define AIR_VALUE 3440
#endif
#ifndef WATER_VALUE
#define WATER_VALUE 1170
#endif

// --- Duty cycle guards ---
#ifndef AWAKE_SECONDS
#define AWAKE_SECONDS 120
#endif
#ifndef SLEEP_SECONDS
#define SLEEP_SECONDS 1680
#endif

const uint64_t SLEEP_US = (uint64_t)SLEEP_SECONDS * 1000000ULL;

// --- BH1750 ---
// Wired to 3V3 permanently — powered down via I2C before deep sleep
// ADDR pin open (default) = 0x23, ADDR pin to VCC = 0x5C
// Decide before potting — cannot change afterward
#define BH1750_ADDR 0x23
#define BH1750_SDA  D4
#define BH1750_SCL  D5
#define LIGHT_SAMPLES 3
#define LIGHT_SAMPLE_DELAY_MS 120  // BH1750 needs ~120ms per measurement

// --- SSD1306 OLED Display ---
// 0.96" 128x64 display (I2C, address 0x3C)
// Connected to same I2C bus as BH1750 (D4/D5)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- Globals ---
WiFiClient           mqttWifiClient;
Adafruit_MQTT_Client mqtt(&mqttWifiClient, AIO_SERVER, AIO_SERVERPORT,
                           AIO_USERNAME, AIO_KEY);
WebServer            server(80);
BH1750               lightMeter;
String               UID;
unsigned long        wakeTime;

// Display state
int lastMoisturePct = 0;
float lastLuxReading = 0;
bool displayInitialized = false;

// --- Node UID ---
String nodeUID() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char uid[5];
  snprintf(uid, sizeof(uid), "%02x%02x", mac[4], mac[5]);
  return String(uid);
}

// --- Feed paths ---
String moistureFeed() {
  return String(AIO_USERNAME) + "/feeds/" + UID + "-soil-moisture";
}

String lightFeed() {
  return String(AIO_USERNAME) + "/feeds/" + UID + "-soil-light";
}

// --- Moisture sensor ---
int readRawADC() {
  int total = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    total += analogRead(MOISTURE_PIN);
    delay(ADC_SAMPLE_DELAY_MS);
  }
  return total / ADC_SAMPLES;
}

int rawToPercent(int raw) {
  return constrain(map(raw, AIR_VALUE, WATER_VALUE, 0, 100), 0, 100);
}

int readMoisture(int &rawOut) {
  digitalWrite(SENSOR_POWER_PIN, HIGH);
  delay(100);
  rawOut = readRawADC();
  digitalWrite(SENSOR_POWER_PIN, LOW);
  return rawToPercent(rawOut);
}

// --- Light sensor ---
float readLux() {
  float total = 0;
  for (int i = 0; i < LIGHT_SAMPLES; i++) {
    float reading = lightMeter.readLightLevel();
    if (reading < 0) {
      Serial.printf("BH1750 read error on sample %d\n", i);
      reading = 0;
    }
    total += reading;
    if (i < LIGHT_SAMPLES - 1) delay(LIGHT_SAMPLE_DELAY_MS);
  }
  return total / LIGHT_SAMPLES;
}

// --- Display Functions ---
// Draw a semicircular gauge with value label
void drawGauge(int x, int y, int radius, int percent, int16_t color, const char*label) {
  // Draw label
  display.setTextSize(1);
  display.setTextColor(color);
  display.setCursor(x - 12, y - radius - 10);
  display.println(label);
  
  // Draw gauge circle (background)
  display.drawCircle(x, y, radius, color);
  
  // Draw filled arc for percentage (approximated with lines)
  int startAngle = 180;  // Start from left
  int endAngle = startAngle + (percent * 180 / 100);  // Sweep to right based on percentage
  
  for (int angle = startAngle; angle <= endAngle; angle += 5) {
    float rad = angle * PI / 180.0;
    int x1 = x + (radius - 2) * cos(rad);
    int y1 = y + (radius - 2) * sin(rad);
    int x2 = x + radius * cos(rad);
    int y2 = y + radius * sin(rad);
    display.drawLine(x1, y1, x2, y2, color);
  }
  
  // Draw percentage/value in center
  display.setTextSize(2);
  display.setTextColor(color);
  char valStr[10];
  snprintf(valStr, sizeof(valStr), "%d%%", percent);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(valStr, x, y, &x1, &y1, &w, &h);
  display.setCursor(x - w/2, y - h/2);
  display.println(valStr);
}

// Draw light level gauge with special formatting 
void drawLightGauge(int x, int y, int radius, float lux) {
  // Scale lux to percentage (0-100000 lux = 0-100%)
  int percent = constrain((int)(lux / 100000.0 * 100), 0, 100);
  
  // Determine color based on light level
  int16_t color = SSD1306_WHITE;
  if (lux < 100) color = SSD1306_WHITE;      // Dark
  else if (lux < 1000) color = SSD1306_WHITE;  // Dim
  else color = SSD1306_WHITE;                   // Bright/Full sun
  
  // Draw label
  display.setTextSize(1);
  display.setTextColor(color);
  display.setCursor(x - 8, y - radius - 10);
  display.println("Light");
  
  // Draw gauge circle (background)
  display.drawCircle(x, y, radius, color);
  
  // Draw filled arc for percentage
  int startAngle = 180;
  int endAngle = startAngle + (percent * 180 / 100);
  
  for (int angle = startAngle; angle <= endAngle; angle += 5) {
    float rad = angle * PI / 180.0;
    int x1 = x + (radius - 2) * cos(rad);
    int y1 = y + (radius - 2) * sin(rad);
    int x2 = x + radius * cos(rad);
    int y2 = y + radius * sin(rad);
    display.drawLine(x1, y1, x2, y2, color);
  }
  
  // Draw lux value with smart formatting
  display.setTextSize(1);
  display.setTextColor(color);
  char valStr[15];
  if (lux >= 1000) {
    snprintf(valStr, sizeof(valStr), "%.1fk", lux / 1000.0);
  } else {
    snprintf(valStr, sizeof(valStr), "%d", (int)lux);
  }
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(valStr, x, y, &x1, &y1, &w, &h);
  display.setCursor(x - w/2, y - 2);
  display.println(valStr);
  
  // Draw unit
  display.setTextSize(1);
  display.setCursor(x - 5, y + 8);
  display.println("lux");
}

// Update display with sensor readings
void updateDisplay(int moisturePct, float lux) {
  if (!displayInitialized) return;
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  // Left side: Moisture gauge (0-50 on x-axis, centered)
  drawGauge(25, 32, 18, moisturePct, SSD1306_WHITE, "Moist");
  
  // Right side: Light gauge (78-128 on x-axis, centered)
  drawLightGauge(101, 32, 18, lux);
  
  display.display();
}

// --- WiFi ---
bool wifiConnect() {
  Serial.printf("Connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.printf("  status: %d\n", WiFi.status());
    attempts++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi failed.");
    return false;
  }
  Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

// --- MQTT ---
bool mqttConnect() {
  if (mqtt.connected()) return true;
  Serial.print("Connecting to Adafruit IO... ");
  int attempts = 0;
  while (mqtt.connect() != 0 && attempts < 3) {
    Serial.print("retry ");
    mqtt.disconnect();
    delay(2000);
    attempts++;
  }
  if (!mqtt.connected()) {
    Serial.println("failed.");
    return false;
  }
  Serial.println("connected.");
  return true;
}

bool publishFloat(const String &feed, float value) {
  // TODO: resilience - buffer failed readings in RTC memory across sleep cycles
  // and flush oldest-first on reconnect. Use cycle count for offline timestamps
  // since SNTP requires WiFi. Note: Adafruit IO free tier timestamps on receipt,
  // so backlog will appear clustered at reconnect time. Consider InfluxDB ingest
  // for accurate historical timestamps when self-hosted stack is in place.
  if (!mqttConnect()) return false;
  Adafruit_MQTT_Publish pub(&mqtt, feed.c_str());
  bool ok = pub.publish(value);
  Serial.printf("MQTT publish to %s: %.1f — %s\n",
                feed.c_str(), value, ok ? "ok" : "failed");
  return ok;
}

// --- SPA ---
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Soil Monitor</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
      background: #1a1a2e;
      color: #eee;
      display: flex;
      justify-content: center;
      align-items: center;
      min-height: 100vh;
    }
    .card {
      background: #16213e;
      border-radius: 16px;
      padding: 2rem;
      text-align: center;
      box-shadow: 0 8px 32px rgba(0,0,0,0.4);
      min-width: 300px;
    }
    .card-header {
      display: flex;
      justify-content: space-between;
      align-items: flex-start;
      margin-bottom: 1.5rem;
    }
    .card-title {
      font-size: 13px;
      font-weight: 500;
      color: #a0aec0;
      text-transform: uppercase;
      letter-spacing: 0.08em;
      text-align: left;
    }
    .uid-badge {
      font-size: 11px;
      font-family: monospace;
      background: #0f3460;
      border: 1px solid #2d3748;
      border-radius: 6px;
      padding: 3px 8px;
      color: #90cdf4;
    }
    .gauges {
      display: flex;
      justify-content: space-around;
      gap: 1rem;
      margin-bottom: 1.25rem;
    }
    .gauge-block { flex: 1; }
    .gauge-wrap {
      position: relative;
      width: 110px;
      height: 110px;
      margin: 0 auto 0.5rem;
    }
    .gauge-wrap svg { transform: rotate(-90deg); display: block; }
    .gauge-text {
      position: absolute;
      inset: 0;
      display: flex;
      flex-direction: column;
      justify-content: center;
      align-items: center;
    }
    .gauge-pct { font-size: 1.6rem; font-weight: 500; line-height: 1; }
    .gauge-unit { font-size: 10px; color: #718096; margin-top: 3px; }
    .gauge-title { font-size: 11px; color: #a0aec0; text-transform: uppercase;
                   letter-spacing: 0.06em; margin-bottom: 0.4rem; }
    .status-label { font-size: 13px; font-weight: 500; }
    .divider {
      border: none;
      border-top: 1px solid #2d3748;
      margin: 1.25rem 0;
    }
    .raw-row {
      display: flex;
      justify-content: space-between;
      font-size: 11px;
      font-family: monospace;
      color: #718096;
      margin-bottom: 0.5rem;
    }
    .updated { font-size: 11px; color: #4a5568; margin-top: 0.5rem; }
    .error { margin-top: 1rem; font-size: 0.8rem; color: #fc8181; min-height: 1.2rem; }
  </style>
</head>
<body>
  <div class="card">
    <div class="card-header">
      <div class="card-title">Soil monitor</div>
      <div class="uid-badge" id="uid-badge">----</div>
    </div>

    <div class="gauges">
      <div class="gauge-block">
        <div class="gauge-title">Moisture</div>
        <div class="gauge-wrap">
          <svg width="110" height="110" viewBox="0 0 110 110">
            <circle cx="55" cy="55" r="44" fill="none" stroke="#2d3748" stroke-width="8"/>
            <circle cx="55" cy="55" r="44" fill="none" id="moisture-arc"
              stroke="#1D9E75" stroke-width="8" stroke-linecap="round"
              stroke-dasharray="276.46" stroke-dashoffset="276.46"/>
          </svg>
          <div class="gauge-text">
            <span class="gauge-pct" id="moisture-pct" style="color:#1D9E75">--</span>
            <span class="gauge-unit">%</span>
          </div>
        </div>
        <div class="status-label" id="moisture-label">--</div>
      </div>

      <div class="gauge-block">
        <div class="gauge-title">Light</div>
        <div class="gauge-wrap">
          <svg width="110" height="110" viewBox="0 0 110 110">
            <circle cx="55" cy="55" r="44" fill="none" stroke="#2d3748" stroke-width="8"/>
            <circle cx="55" cy="55" r="44" fill="none" id="light-arc"
              stroke="#EF9F27" stroke-width="8" stroke-linecap="round"
              stroke-dasharray="276.46" stroke-dashoffset="276.46"/>
          </svg>
          <div class="gauge-text">
            <span class="gauge-pct" id="light-pct" style="color:#EF9F27">--</span>
            <span class="gauge-unit">lux</span>
          </div>
        </div>
        <div class="status-label" id="light-label">--</div>
      </div>
    </div>

    <hr class="divider">
    <div class="raw-row">
      <span>moisture raw</span>
      <span id="moisture-raw">--</span>
    </div>
    <div class="raw-row">
      <span>light raw</span>
      <span id="light-raw">--</span>
    </div>
    <div class="updated" id="updated"></div>
    <div class="error" id="error"></div>
  </div>

  <script>
    const MOISTURE_CIRC = 276.46;
    const LIGHT_CIRC    = 276.46;
    const MAX_LUX       = 100000;

    function getMoistureColor(p) {
      if (p < 20) return '#E24B4A';
      if (p < 40) return '#EF9F27';
      if (p < 70) return '#1D9E75';
      return '#378ADD';
    }
    function getMoistureLabel(p) {
      if (p < 20) return 'Dry';
      if (p < 40) return 'Low';
      if (p < 70) return 'Good';
      return 'Wet';
    }
    function getLightColor(p) {
      if (p < 10) return '#4a5568';
      if (p < 30) return '#EF9F27';
      return '#FAC75A';
    }
    function getLightLabel(lux) {
      if (lux < 100)   return 'Dark';
      if (lux < 1000)  return 'Dim';
      if (lux < 10000) return 'Bright';
      return 'Full sun';
    }

    function updateArc(arcId, circ, pct, color) {
      const arc = document.getElementById(arcId);
      arc.style.stroke = color;
      arc.setAttribute('stroke-dashoffset', circ * (1 - pct / 100));
    }

    function updateUI(data) {
      // Moisture
      const mp = data.moisture_pct;
      const mc = getMoistureColor(mp);
      updateArc('moisture-arc', MOISTURE_CIRC, mp, mc);
      const mpEl = document.getElementById('moisture-pct');
      mpEl.textContent = mp;
      mpEl.style.color = mc;
      document.getElementById('moisture-label').textContent = getMoistureLabel(mp);
      document.getElementById('moisture-raw').textContent = data.moisture_raw;

      // Light
      const lux = data.light_lux;
      const luxPct = Math.min(lux / MAX_LUX * 100, 100);
      const lc = getLightColor(luxPct);
      updateArc('light-arc', LIGHT_CIRC, luxPct, lc);
      const lpEl = document.getElementById('light-pct');
      lpEl.textContent = lux >= 1000
        ? (lux / 1000).toFixed(1) + 'k'
        : Math.round(lux);
      lpEl.style.color = lc;
      document.getElementById('light-label').textContent = getLightLabel(lux);
      document.getElementById('light-raw').textContent = lux.toFixed(1) + ' lx';

      document.getElementById('uid-badge').textContent = data.uid;
      document.getElementById('updated').textContent =
        'updated ' + new Date().toLocaleTimeString();
      document.getElementById('error').textContent = '';
    }

    async function fetchStatus() {
      try {
        const res = await fetch('/status');
        const data = await res.json();
        updateUI(data);
      } catch(e) {
        document.getElementById('error').textContent = 'Could not reach device';
      }
    }

    fetchStatus();
    setInterval(fetchStatus, 2000);
  </script>
</body>
</html>
)rawliteral";

// --- Web routes ---
void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  int raw = 0;
  int pct = readMoisture(raw);
  float lux = readLux();

  // Update display with latest readings
  lastMoisturePct = pct;
  lastLuxReading = lux;
  updateDisplay(pct, lux);

  String json = "{";
  json += "\"moisture_pct\":"  + String(pct)         + ",";
  json += "\"moisture_raw\":"  + String(raw)          + ",";
  json += "\"light_lux\":"     + String(lux, 1)       + ",";
  json += "\"uid\":\""         + UID                  + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  delay(2000);

  UID = nodeUID();
  Serial.printf("Node UID: %s\n", UID.c_str());

  // Moisture sensor power pin
  pinMode(SENSOR_POWER_PIN, OUTPUT);
  digitalWrite(SENSOR_POWER_PIN, LOW);
  analogSetAttenuation(ADC_11db);

  // BH1750 on I2C
  Wire.begin(BH1750_SDA, BH1750_SCL);
  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, BH1750_ADDR)) {
    Serial.println("BH1750 not found — check wiring");
  } else {
    Serial.println("BH1750 ready");
  }

  // SSD1306 OLED display on same I2C bus
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 not found — check wiring");
    displayInitialized = false;
  } else {
    Serial.println("SSD1306 ready");
    displayInitialized = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 28);
    display.println("Soil Monitor");
    display.display();
    delay(1000);
  }

  // WiFi
  bool online = wifiConnect();

  // Read sensors
  int raw = 0;
  int pct = readMoisture(raw);
  float lux = readLux();
  Serial.printf("Moisture: %d%% (raw: %d)\n", pct, raw);
  Serial.printf("Light: %.1f lux\n", lux);

  // Update OLED display
  lastMoisturePct = pct;
  lastLuxReading = lux;
  updateDisplay(pct, lux);

  // Publish if online
  if (online) {
    publishFloat(moistureFeed(), (float)pct);
    publishFloat(lightFeed(), lux);
  }

  // Power down BH1750 before sleep
  Wire.beginTransmission(BH1750_ADDR);
  // Wire.write(0x00); // Power down command (not needed here, I want it to read continuously until sleep)
  Wire.endTransmission();

  // Start webserver for remainder of awake window
  server.on("/", handleRoot);
  server.on("/favicon.ico", []() { server.send(204); });
  server.on("/status", handleStatus);
  server.begin();
  Serial.printf("Webserver up — sleeping in %ds\n", AWAKE_SECONDS);

  wakeTime = millis();
}

// --- Loop ---
void loop() {
  server.handleClient();

  // Periodically refresh display with last readings
  static unsigned long lastDisplayUpdate = 0;
  if (millis() - lastDisplayUpdate >= 2000) {  // Update every 2 seconds
    updateDisplay(lastMoisturePct, lastLuxReading);
    lastDisplayUpdate = millis();
  }

  if (millis() - wakeTime >= (uint32_t)AWAKE_SECONDS * 1000) {
    Serial.println("Entering deep sleep...");
    Wire.beginTransmission(BH1750_ADDR);
    Wire.write(0x00); // Power down command
    Wire.endTransmission();
    Serial.flush();
    esp_sleep_enable_timer_wakeup(SLEEP_US);
    esp_deep_sleep_start();
  }
}