#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <BH1750.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"

// --- Display Settings ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1 
#define SCREEN_ADDRESS 0x3C // Common for 0.96" OLED
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

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
#ifndef AIR_VALUE
#define AIR_VALUE 3440
#endif
#ifndef WATER_VALUE
#define WATER_VALUE 1170
#endif

// --- BH1750 ---
#define BH1750_ADDR 0x23
#define BH1750_SDA  D4
#define BH1750_SCL  D5

// --- Globals ---
WiFiClient           mqttWifiClient;
Adafruit_MQTT_Client mqtt(&mqttWifiClient, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);
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

// --- Helper: Draw Gauge ---
// Draws a semi-circular arc gauge with a label
void drawGauge(int x, int y, int radius, int percent, const char* label, String value) {
  // Draw base arc (dots or thin line)
  display.drawCircleHelper(x, y, radius, 1, SSD1306_WHITE); 
  display.drawCircleHelper(x, y, radius, 2, SSD1306_WHITE);

  // Calculate end point of the "needle" based on percentage (0-100)
  // Mapping 180 degrees (from left to right)
  float angle = map(percent, 0, 100, 180, 360) * DEG_TO_RAD;
  int lx = x + cos(angle) * radius;
  int ly = y + sin(angle) * radius;
  display.drawLine(x, y, lx, ly, SSD1306_WHITE);

  // Text
  display.setTextSize(1);
  display.setCursor(x - (radius), y + 5);
  display.print(label);
  
  display.setCursor(x - (radius), y + 15);
  display.print(value);
}

void updateOLED(int moisture, float lux) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  // Header
  display.setTextSize(1);
  display.setCursor(0,0);
  display.print("ID: "); display.print(UID);
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  // Moisture Gauge (Left)
  drawGauge(30, 40, 25, moisture, "MOIST", String(moisture) + "%");

  // Light Gauge (Right)
  // Map lux to 0-100 for the arc (10k lux as "full")
  int luxPercent = constrain(map((int)lux, 0, 10000, 0, 100), 0, 100);
  String luxStr = lux >= 1000 ? String(lux/1000, 1) + "k" : String((int)lux);
  drawGauge(95, 40, 25, luxPercent, "LIGHT", luxStr + "lx");

  display.display();
}

// --- Sensors & WiFi Logic (Keep from your original code) ---
int readRawADC() {
  int total = 0;
  for (int i = 0; i < 10; i++) { total += analogRead(MOISTURE_PIN); delay(5); }
  return total / 10;
}

int readMoisture(int &rawOut) {
  digitalWrite(SENSOR_POWER_PIN, HIGH);
  delay(100);
  rawOut = readRawADC();
  digitalWrite(SENSOR_POWER_PIN, LOW);
  return constrain(map(rawOut, AIR_VALUE, WATER_VALUE, 0, 100), 0, 100);
}

float readLux() {
  float lux = lightMeter.readLightLevel();
  return (lux < 0) ? 0 : lux;
}

bool mqttConnect() {
  if (mqtt.connected()) return true;
  if (mqtt.connect() == 0) return true;
  return false;
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  UID = nodeUID();

  pinMode(SENSOR_POWER_PIN, OUTPUT);
  digitalWrite(SENSOR_POWER_PIN, LOW);
  analogSetAttenuation(ADC_11db);

  // Init I2C
  Wire.begin(BH1750_SDA, BH1750_SCL);

  // Init OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
  }
  display.clearDisplay();
  display.display();

  // Init BH1750
  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, BH1750_ADDR);

  // Connect WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  // Initial Reading
  int raw = 0;
  int pct = readMoisture(raw);
  float lux = readLux();

  // Show on OLED immediately
  updateOLED(pct, lux);

  // MQTT Publish
  if (WiFi.status() == WL_CONNECTED && mqttConnect()) {
    Adafruit_MQTT_Publish p1(&mqtt, (String(AIO_USERNAME) + "/feeds/" + UID + "-soil-moisture").c_str());
    Adafruit_MQTT_Publish p2(&mqtt, (String(AIO_USERNAME) + "/feeds/" + UID + "-soil-light").c_str());
    p1.publish((double)pct);
    p2.publish((double)lux);
  }

  // Start Webserver
  server.on("/", []() { server.send(200, "text/html", "Refer to your original SPA HTML here"); });
  server.on("/status", []() {
    int r; int p = readMoisture(r); float l = readLux();
    String json = "{\"moisture_pct\":" + String(p) + ",\"light_lux\":" + String(l,1) + "}";
    server.send(200, "application/json", json);
    updateOLED(p, l); // Update screen when someone hits the web API too
  });
  server.begin();
  
  wakeTime = millis();
}

void loop() {
  server.handleClient();

  // Periodic OLED update during awake time (every 5 seconds)
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 5000) {
    int r;
    updateOLED(readMoisture(r), readLux());
    lastUpdate = millis();
  }

  // Deep Sleep Logic
  if (millis() - wakeTime >= 120000) { // 120 seconds
    display.clearDisplay();
    display.display(); // Turn off pixels before sleep
    esp_sleep_enable_timer_wakeup(1680ULL * 1000000ULL);
    esp_deep_sleep_start();
  }
}