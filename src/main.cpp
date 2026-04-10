#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <BH1750.h>
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

// --- Globals ---
WiFiClient           mqttWifiClient;
Adafruit_MQTT_Client mqtt(&mqttWifiClient, AIO_SERVER, AIO_SERVERPORT,
                           AIO_USERNAME, AIO_KEY);
WebServer            server(80);
BH1750               lightMeter;
String               UID;
unsigned long        wakeTime;

// --- Node UID ---
String nodeUID() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char uid[5];
  snprintf(uid, sizeof(uid), "%02x%02x", mac[4], mac[5]);
  return String(uid);
}

// --- Feed paths ---
String moistureFeedPath() {
  return String(AIO_USERNAME) + "/feeds/" + UID + "-soil-moisture";
}

String lightFeedPath() {
  return String(AIO_USERNAME) + "/feeds/" + UID + "-light-level";
}

// --- Sensor ---
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

bool publishMoisture(int pct) {
  // TODO: resilience - buffer failed readings in RTC memory across sleep cycles
  // and flush oldest-first on reconnect. Use cycle count for offline timestamps
  // since SNTP requires WiFi. Note: Adafruit IO free tier timestamps on receipt,
  // so backlog will appear clustered at reconnect time. Consider InfluxDB ingest
  // for accurate historical timestamps when self-hosted stack is in place.
  if (!mqttConnect()) return false;
  String path = moistureFeedPath();
  Adafruit_MQTT_Publish feed(&mqtt, path.c_str());
  bool ok = feed.publish((int32_t)pct);
  Serial.printf("MQTT publish to %s: %d%% — %s\n",
                path.c_str(), pct, ok ? "ok" : "failed");
  return ok;
}

bool publishLight(float lux) {
  if (!mqttConnect()) return false;
  String path = lightFeedPath();
  Adafruit_MQTT_Publish feed(&mqtt, path.c_str());
  bool ok = feed.publish(lux);
  Serial.printf("MQTT publish to %s: %.1f lx — %s\n",
                path.c_str(), lux, ok ? "ok" : "failed");
  return ok;
}

// --- SPA ---
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Soil Moisture</title>
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
      min-width: 280px;
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
    .gauge-wrap {
      position: relative;
      width: 140px;
      height: 140px;
      margin: 0 auto 1.25rem;
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
    .gauge-pct { font-size: 2rem; font-weight: 500; line-height: 1; }
    .gauge-unit { font-size: 11px; color: #718096; margin-top: 3px; }
    .status-label { font-size: 15px; font-weight: 500; margin-bottom: 4px; }
    .raw-value { font-size: 12px; font-family: monospace; color: #718096; margin-bottom: 1rem; }
    .updated { font-size: 11px; color: #4a5568; }
    .error { margin-top: 1rem; font-size: 0.8rem; color: #fc8181; min-height: 1.2rem; }
  </style>
</head>
<body>
  <div class="card">
    <div class="card-header">
      <div class="card-title">Soil moisture</div>
      <div class="uid-badge" id="uid-badge">----</div>
    </div>
    <div class="gauge-wrap">
      <svg width="140" height="140" viewBox="0 0 140 140">
        <circle cx="70" cy="70" r="56" fill="none" stroke="#2d3748" stroke-width="10"/>
        <circle cx="70" cy="70" r="56" fill="none" id="gauge-arc"
          stroke="#1D9E75" stroke-width="10" stroke-linecap="round"
          stroke-dasharray="351.86" stroke-dashoffset="351.86"/>
      </svg>
      <div class="gauge-text">
        <span class="gauge-pct" id="pct" style="color:#1D9E75">--</span>
        <span class="gauge-unit">% moisture</span>
      </div>
    </div>
    <div class="status-label" id="status-label">Reading...</div>
    <div class="light-value" id="light-value" style="font-size: 14px; margin-bottom: 0.5rem; color: #fbd38d;"></div>
    <div class="raw-value" id="raw-value"></div>
    <div class="updated" id="updated"></div>
    <div class="error" id="error"></div>
  </div>
  <script>
    const CIRC = 351.86;
    function getColor(p) {
      if (p < 20) return '#E24B4A';
      if (p < 40) return '#EF9F27';
      if (p < 70) return '#1D9E75';
      return '#378ADD';
    }
    function getLabel(p) {
      if (p < 20) return 'Dry';
      if (p < 40) return 'Low';
      if (p < 70) return 'Good';
      return 'Wet';
    }
    function updateUI(data) {
      const p = data.moisture_pct;
      const color = getColor(p);
      const arc = document.getElementById('gauge-arc');
      arc.style.stroke = color;
      arc.setAttribute('stroke-dashoffset', CIRC * (1 - p / 100));
      const pctEl = document.getElementById('pct');
      pctEl.textContent = p;
      pctEl.style.color = color;
      document.getElementById('status-label').textContent = getLabel(p);
      document.getElementById('light-value').textContent = 'Light intensity: ' + data.light_lux.toFixed(1) + ' lx';
      document.getElementById('raw-value').textContent = 'raw ADC: ' + data.moisture_raw;
      document.getElementById('updated').textContent =
        'updated ' + new Date().toLocaleTimeString();
      document.getElementById('uid-badge').textContent = data.uid;
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
  float lux = lightMeter.readLightLevel();
  String json = "{\"moisture_pct\":";
  json += pct;
  json += ",\"moisture_raw\":";
  json += raw;
  json += ",\"light_lux\":";
  json += lux;
  json += ",\"uid\":\"";
  json += UID;
  json += "\"}";
  server.send(200, "application/json", json);
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  delay(1000);

  UID = nodeUID();
  Serial.printf("Node UID: %s\n", UID.c_str());

  pinMode(SENSOR_POWER_PIN, OUTPUT);
  digitalWrite(SENSOR_POWER_PIN, LOW);
  analogSetAttenuation(ADC_11db);

  Wire.begin();
  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println(F("BH1750 initialised"));
  } else {
    Serial.println(F("Error initialising BH1750"));
  }

  bool online = wifiConnect();

  int raw = 0;
  int pct = readMoisture(raw);
  Serial.printf("Moisture: %d%% (raw: %d)\n", pct, raw);

  float lux = lightMeter.readLightLevel();
  Serial.printf("Light: %.1f lx\n", lux);

  if (online) {
    publishMoisture(pct);
    publishLight(lux);
  }

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

  if (millis() - wakeTime >= (uint32_t)AWAKE_SECONDS * 1000) {
    Serial.println("Entering deep sleep...");
    // BH1750 uses UNCONFIGURED (0x00) as the power-down state.
    lightMeter.configure(BH1750::UNCONFIGURED);
    Serial.flush();
    esp_sleep_enable_timer_wakeup(SLEEP_US);
    esp_deep_sleep_start();
  }
}