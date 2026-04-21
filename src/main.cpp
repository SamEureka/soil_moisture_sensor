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
#define MOISTURE_PIN D0 // analog pin for moisture sensor
#endif
#ifndef SENSOR_POWER_PIN
#define SENSOR_POWER_PIN D2 // powers the moisture sensor via MOSFET to avoid idle current draw
#endif

// --- Battery voltage divider guards ---
#ifndef VBAT_PIN
#define VBAT_PIN D10 // 1MΩ voltage divider from battery to ADC pin
#endif
#ifndef VBAT_POWER_PIN
#define VBAT_POWER_PIN D8 // 2N7000 gate to switch divider for measurement
#endif
#ifndef VBAT_ADC_MV_MAX
#define VBAT_ADC_MV_MAX 2450.0f // mV at ADC count 4095 (11dB, empirically ~2450)
#endif
#ifndef VBAT_SCALE
#define VBAT_SCALE 2.0f // restore divided voltage (1M/1M = ÷2)/ scale factor for voltage divider (1M /
#endif
#ifndef VBAT_FULL
#define VBAT_FULL 4.20f // mV corresponding to 100% battery level (empirically ~4.2V)
#endif
#ifndef VBAT_EMPTY
#define VBAT_EMPTY 3.20f // mV corresponding to 0% battery level (empirically ~3.0V)
#endif
#ifndef VBAT_SAMPLES
#define VBAT_SAMPLES 10 // number of ADC samples to average for battery voltage reading  
#endif
#ifndef VBAT_SAMPLE_DELAY_MS
#define VBAT_SAMPLE_DELAY_MS 10 // delay between ADC samples for battery voltage reading 
#endif
#ifndef VBAT_POWER_SETTLE_MS
#define VBAT_POWER_SETTLE_MS 50 // delay after enabling voltage divider before taking ADC readings 
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

// --- Display ---
#ifndef OLED_BRIGHTNESS
#define OLED_BRIGHTNESS 192
#endif
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDR     0x3C
#define SLEEP_WARN_SECONDS 5

// --- BH1750 ---
#define BH1750_ADDR        0x23  // ADDR pin floating — defaults to 0x23
#define BH1750_SDA         D4
#define BH1750_SCL         D5
#define LIGHT_SAMPLES      3
#define LIGHT_SAMPLE_DELAY_MS 120

const uint64_t SLEEP_US = (uint64_t)SLEEP_SECONDS * 1000000ULL;

// --- Globals ---
WiFiClient              mqttWifiClient;
Adafruit_MQTT_Client    mqtt(&mqttWifiClient, AIO_SERVER, AIO_SERVERPORT,
                              AIO_USERNAME, AIO_KEY);
WebServer               server(80);
Adafruit_SSD1306        display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
BH1750                  lightMeter;
String                  UID;
unsigned long           wakeTime;
int                     lastMoisturePct = 0;
int                     lastMoistureRaw = 0;
float                   lastLux         = 0;
float                   lastBatteryV  = 0.0f;
int                     lastBatteryPct = 0;

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
String batteryFeed() {
  return String(AIO_USERNAME) + "/feeds/" + UID + "-soil-battery";
}

// --- Sensor reading ---
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

float readLux() {
  float total = 0;
  for (int i = 0; i < LIGHT_SAMPLES; i++) {
    float reading = lightMeter.readLightLevel();
    if (reading < 0) reading = 0;
    total += reading;
    if (i < LIGHT_SAMPLES - 1) delay(LIGHT_SAMPLE_DELAY_MS);
  }
  return total / LIGHT_SAMPLES;
}

// read battery voltage by enabling power to divider, taking ADC readings, then disabling power to save current
float readBatter(int &pctOut) {
  pinMode(VBAT_POWER_PIN, OUTPUT);
  digitalWrite(VBAT_POWER_PIN, HIGH);
  delay(VBAT_POWER_SETTLE_MS);

  uint32_t total = 0;
  for (int i = 0; i < VBAT_SAMPLES; i++) {
    total += analogRead(VBAT_PIN);
    delay(VBAT_SAMPLE_DELAY_MS);
  } 
  digitalWrite(VBAT_POWER_PIN, LOW); // kill divider power to save current

  float raw = (float)total / VBAT_SAMPLES;
  float adcMv = raw * (VBAT_ADC_MV_MAX / 4095.0f); // convert ADC count to mV 
  float battV = (adcMv /1000.0f) * VBAT_SCALE; // restore actual battery voltage from divided voltage 

  pctOut = (int)constrain(
    ((battV - VBAT_EMPTY) / (VBAT_FULL - VBAT_EMPTY)) * 100.0f,
    0.0f, 100.0f
  );
  return battV;
}


// --- Display helpers ---

// Draw a semicircle arc gauge
// cx, cy = center, r = radius, value = 0-100, color = 1
void drawArc(int cx, int cy, int r, int valuePct) {
  // arc sweeps from 180° to 0° (left to right across bottom half)
  // 180° = fully empty, 0° = fully full
  float startAngle = PI;       // 180 degrees — left
  float endAngle   = 0;        // 0 degrees   — right
  float filled     = startAngle - (startAngle - endAngle) * (valuePct / 100.0);

  // draw background arc (empty)
  for (float a = 0; a <= PI; a += 0.05) {
    int x = cx + (int)(r * cos(PI - a));
    int y = cy - (int)(r * sin(a));
    display.drawPixel(x, y, SSD1306_WHITE);
  }

  // draw second ring for thickness
  for (float a = 0; a <= PI; a += 0.05) {
    int x = cx + (int)((r - 1) * cos(PI - a));
    int y = cy - (int)((r - 1) * sin(a));
    display.drawPixel(x, y, SSD1306_WHITE);
  }

  // overdraw filled portion in BLACK to create empty look
  float fillAngle = PI * (1.0 - valuePct / 100.0);
  for (float a = 0; a <= fillAngle; a += 0.05) {
    int x = cx + (int)(r * cos(PI - a));
    int y = cy - (int)(r * sin(a));
    display.drawPixel(x, y, SSD1306_BLACK);
    x = cx + (int)((r - 1) * cos(PI - a));
    y = cy - (int)((r - 1) * sin(a));
    display.drawPixel(x, y, SSD1306_BLACK);
  }

  // filled arc on top
  float fillEnd = PI * (valuePct / 100.0);
  for (float a = 0; a <= fillEnd; a += 0.04) {
    float angle = PI - a;
    for (int t = -2; t <= 2; t++) {
      int x = cx + (int)((r + t * 0.3) * cos(angle));
      int y = cy - (int)((r + t * 0.3) * sin(PI - angle));
      display.drawPixel(x, y, SSD1306_WHITE);
    }
  }
}

// Format lux for display — 42300 → "42.3k", 850 → "850"
String formatLux(float lux) {
  if (lux >= 1000) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%.1fk", lux / 1000.0);
    return String(buf);
  }
  return String((int)lux);
}

// Moisture status label
const char* moistureLabel(int pct) {
  if (pct < 20) return "Dry";
  if (pct < 40) return "Low";
  if (pct < 70) return "Good";
  return "Wet";
}

// Light status label
const char* lightLabel(float lux) {
  if (lux < 100)   return "Dark";
  if (lux < 1000)  return "Dim";
  if (lux < 10000) return "Bright";
  return "Full sun";
}

// Map lux to 0-100 for arc display (log scale feels more natural)
int luxToPercent(float lux) {
  if (lux <= 0) return 0;
  // log scale: 1 lux = ~1%, 100k lux = 100%
  float pct = (log10(lux + 1) / log10(100001)) * 100.0;
  return constrain((int)pct, 0, 100);
}

void updateDisplay() {
  display.clearDisplay();

  unsigned long elapsed = (millis() - wakeTime) / 1000;
  int remaining = (int)AWAKE_SECONDS - (int)elapsed;
  if (remaining < 0) remaining = 0;

  bool sleepWarning = (remaining <= SLEEP_WARN_SECONDS);

  // --- section titles ---
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  display.print("MOISTURE");
  display.setCursor(72, 0);
  display.print("LIGHT");

  // --- arc gauges ---
  // moisture arc: center x=28, y=40, radius=22
  int mPct   = lastMoisturePct;
  int lPct   = luxToPercent(lastLux);

  drawArc(28, 42, 22, mPct);
  drawArc(98, 42, 22, lPct);

  // --- values centered in arcs ---
  display.setTextSize(1);

  // moisture percent
  String mStr = String(mPct) + "%";
  int mStrW = mStr.length() * 6;
  display.setCursor(28 - mStrW / 2, 30);
  display.print(mStr);

  // lux value
  String lStr = formatLux(lastLux);
  int lStrW = lStr.length() * 6;
  display.setCursor(98 - lStrW / 2, 30);
  display.print(lStr);

  // --- status labels below arcs ---
  display.setTextSize(1);
  const char* mLabel = moistureLabel(mPct);
  int mLabelW = strlen(mLabel) * 6;
  display.setCursor(28 - mLabelW / 2, 44);
  display.print(mLabel);

  const char* lLabel = lightLabel(lastLux);
  int lLabelW = strlen(lLabel) * 6;
  display.setCursor(98 - lLabelW / 2, 44);
  display.print(lLabel);

  // --- bottom bar ---
  // divider line above bottom bar
  display.drawFastHLine(0, 53, SCREEN_WIDTH, SSD1306_WHITE);

  if (sleepWarning) {
    // sleep warning message
    display.setCursor(2, 56);
    display.print(UID);
    display.setCursor(34, 56);
    char sleepMsg[20];
    snprintf(sleepMsg, sizeof(sleepMsg), "sleep %dm", SLEEP_SECONDS / 60);
    display.print(sleepMsg);
} else {
    // UID on left
    display.setCursor(2, 56);
    display.print(UID);

    // battery voltage right-justified (e.g. "3.87V")
    char batStr[8];
    snprintf(batStr, sizeof(batStr), "%.2fV", lastBatteryV);
    int batW = strlen(batStr) * 6;
    display.setCursor(SCREEN_WIDTH - batW - 1, 56);
    display.print(batStr);

    // progress bar between UID and voltage — shrink width to leave room for "X.XXV"
    // UID ends around x=26, voltage label is ~batW wide at right edge
    int barX = 28;
    int barY = 56;
    int barW = SCREEN_WIDTH - barX - batW - 4;   // 4px gutter
    int barH = 6;
    if (barW > 4) {
      display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
      int filledW = (int)(barW * (float)remaining / (float)AWAKE_SECONDS);
      if (filledW > 2) {
        display.fillRect(barX + 1, barY + 1, filledW - 2, barH - 2, SSD1306_WHITE);
      }
    }
  }

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
    .divider { border: none; border-top: 1px solid #2d3748; margin: 1.25rem 0; }
    .bat-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      font-size: 12px;
      font-family: monospace;
      color: #a0aec0;
      margin-bottom: 0.75rem;
    }
    .bat-bar-wrap {
      flex: 1;
      margin: 0 0.75rem;
      height: 6px;
      background: #2d3748;
      border-radius: 3px;
      overflow: hidden;
    }
    .bat-bar-fill {
      height: 100%;
      border-radius: 3px;
      transition: width 0.5s ease, background 0.5s ease;
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
    <div class="bat-row">
      <span>&#x1F50B;</span>
      <div class="bat-bar-wrap">
        <div class="bat-bar-fill" id="bat-bar" style="width:0%"></div>
      </div>
      <span id="bat-label" style=font-family:monospace;font-size:11px;>--</span>" 
    </div>    
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
      const mp = data.moisture_pct;
      const mc = getMoistureColor(mp);
      updateArc('moisture-arc', MOISTURE_CIRC, mp, mc);
      const mpEl = document.getElementById('moisture-pct');
      mpEl.textContent = mp;
      mpEl.style.color = mc;
      document.getElementById('moisture-label').textContent = getMoistureLabel(mp);
      document.getElementById('moisture-raw').textContent = data.moisture_raw;
      const lux = data.light_lux;
      const luxPct = Math.min(Math.log10(lux + 1) / Math.log10(100001) * 100, 100);
      const lc = getLightColor(luxPct);
      updateArc('light-arc', LIGHT_CIRC, luxPct, lc);
      const lpEl = document.getElementById('light-pct');
      lpEl.textContent = lux >= 1000
        ? (lux / 1000).toFixed(1) + 'k'
        : Math.round(lux);
      lpEl.style.color = lc;
      document.getElementById('light-label').textContent = getLightLabel(lux);
      document.getElementById('light-raw').textContent = lux.toFixed(1) + ' lx';
      // battery
      const bv  = data.battery_v;
      const bpc = data.battery_pct;
      const batColor = bpc > 50 ? '#1D9E75' : bpc > 20 ? '#EF9F27' : '#E24B4A';
      const batBar = document.getElementById('bat-bar');
      batBar.style.width  = bpc + '%';
      batBar.style.background = batColor;
      document.getElementById('bat-label').textContent =
        bv.toFixed(2) + 'V (' + bpc + '%)';
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
  int raw  = 0;
  int pct  = readMoisture(raw);
  float lux = readLux();
  float battV = readBatter(/* pctOut */ lastBatteryPct);
  lastBatteryV = battV;

  // update globals so display reflects latest SPA-triggered reading
  lastMoisturePct = pct;
  lastMoistureRaw = raw;
  lastLux         = lux;

  String json = "{";
  json += "\"moisture_pct\":"  + String(pct)   + ",";
  json += "\"moisture_raw\":"  + String(raw)    + ",";
  json += "\"light_lux\":"     + String(lux, 1) + ",";
  json += "\"battery_v\":"     + String(lastBatteryV, 2) + ",";
  json += "\"battery_pct\":"   + String(lastBatteryPct) + ",";
  json += "\"uid\":\""         + UID            + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  delay(2000);

  UID = nodeUID();
  Serial.printf("Node UID: %s\n", UID.c_str());

  pinMode(SENSOR_POWER_PIN, OUTPUT);
  digitalWrite(SENSOR_POWER_PIN, LOW);
  analogSetAttenuation(ADC_11db);

  // I2C + BH1750
  Wire.begin(BH1750_SDA, BH1750_SCL);
  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, BH1750_ADDR)) {
    Serial.println("BH1750 not found — check wiring");
  } else {
    Serial.println("BH1750 ready");
  }

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 not found — check wiring");
  } else {
    display.ssd1306_command(0x81);         // set contrast command
    display.ssd1306_command(OLED_BRIGHTNESS);
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 24);
    display.print("soil monitor");
    display.setCursor(38, 36);
    display.print("node: ");
    display.print(UID);  // can't show UID yet, but setup splash
    display.display();
    Serial.println("SSD1306 ready");
  }

  // WiFi
  bool online = wifiConnect();

  // Read sensors
  lastMoistureRaw = 0;
  lastMoisturePct = readMoisture(lastMoistureRaw);
  lastLux         = readLux();
  lastBatteryV    = readBatter(lastBatteryPct);
  Serial.printf("Moisture: %d%% (raw: %d)\n", lastMoisturePct, lastMoistureRaw);
  Serial.printf("Light: %.1f lux\n", lastLux);
  Serial.printf("Battery: %.2f V (%d%%)\n", lastBatteryV, lastBatteryPct);

  // Publish
  if (online) {
    publishFloat(moistureFeed(), (float)lastMoisturePct);
    publishFloat(lightFeed(), lastLux);
    publishFloat(batteryFeed(), lastBatteryPct);
  }

  // Start webserver
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
  updateDisplay();

  if (millis() - wakeTime >= (uint32_t)AWAKE_SECONDS * 1000) {
    Serial.println("Entering deep sleep...");
    display.clearDisplay();
    display.display();
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    Wire.beginTransmission(BH1750_ADDR);
    Wire.write(0x00);  // BH1750 power down opcode
    Wire.endTransmission();
    Serial.flush();
    esp_sleep_enable_timer_wakeup(SLEEP_US);
    esp_deep_sleep_start();
  }
}