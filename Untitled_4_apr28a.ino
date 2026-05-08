#include "arduino_secrets.h"
// ============================================================
//  AgroCloud v2.3 – Intelligent Greenhouse Climate & Irrigation
//  ESP32 + Arduino IoT Cloud
//
//  GÜNCELLEMELER (v2.3):
//    ✅ NEW: Sıcaklık ve Nem Bildirimleri (Environmental Alarms)
//    ✅ FIX: Cloud Senkronizasyon (Race Condition) Çözüldü
//    ✅ FIX: OLED Ekran çakışmaları ve ID gösterimi eklendi
// ============================================================

#include "thingProperties.h"
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>

// --- PIN TANIMLAR ---
#define DHTPIN      5
#define DHTTYPE     DHT11
#define SOIL_PIN    34
#define RELAY_PIN   26

// --- OLED ---
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

DHT dht(DHTPIN, DHTTYPE);

// ============================================================
//  HAREKETLİ ORTALAMA — Sensör Bounce Koruma
// ============================================================
#define MOISTURE_SAMPLES 3
float moistureBuffer[MOISTURE_SAMPLES] = {50, 50, 50};
int   moistureBufferIdx = 0;

float getSmoothedMoisture(float newReading) {
  moistureBuffer[moistureBufferIdx] = newReading;
  moistureBufferIdx = (moistureBufferIdx + 1) % MOISTURE_SAMPLES;
  float sum = 0;
  for (int i = 0; i < MOISTURE_SAMPLES; i++) sum += moistureBuffer[i];
  return sum / MOISTURE_SAMPLES;
}

// ============================================================
//  MİNİMUM ÇALIŞMA SÜRESİ — Relay Koruma
// ============================================================
#define MIN_PUMP_MS 3000UL
unsigned long pumpStartTime = 0;
bool          pumpIsRunning = false;

// ============================================================
//  BİTKİ PRESETLERİ
// ============================================================
struct PlantProfile {
  const char* name;
  int   moistureMin;    
  int   moistureTarget; 
  int   moistureMax;    
  float stressTempC;
  float stressHumidity;
  float dangerTempC;
};

const PlantProfile PLANTS[] = {
  { "CUSTOM",    20,     50,  80,   30.0f,   40.0f,   35.0f },  // 0
  { "CACTUS",     5,     15,  30,   45.0f,   20.0f,   50.0f },  // 1
  { "TOMATO",    30,     60,  85,   28.0f,   45.0f,   33.0f },  // 2
  { "ORCHID",    40,     65,  80,   25.0f,   50.0f,   30.0f },  // 3
};

PlantProfile activeProfile = PLANTS[0];

// ============================================================
//  NON-BLOCKING ZAMANLAMA VE SYNC GUARD
// ============================================================
unsigned long lastSensorRead = 0;
const unsigned long SENSOR_INTERVAL = 2000;

bool localPumpDesired = false;
bool prevPumpStatus   = false;

unsigned long lastPresetChangeTime = 0;
const unsigned long SYNC_GUARD_MS = 3000; 

// ============================================================
//  OLED CACHE
// ============================================================
struct OledCache {
  int   temp       = -999;
  int   hum        = -999;
  int   soil       = -999;
  bool  autoMode   = false;
  bool  pump       = false;
  int   threshold  = -1;
  char  plant[15]  = "";
  int   rssi       = 0;
  bool  iotConn    = false;
};
OledCache oledCache;
bool      oledInitDone = false;

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(9600); // Daha standart hız (Web Editor için)
  delay(1500);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);  

  dht.begin();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED baslatilamadi"));
  }
  display.clearDisplay();
  display.display();

  initProperties();
  ArduinoCloud.begin(ArduinoIoTPreferredConnection);
  
  // applyPlantPreset(0); // <-- SİLDİM: Dashboard ayarı korunsun
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  ArduinoCloud.update();

  unsigned long now = millis();
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readSensors();
    runControlLogic();
    applyRelay();
    checkPushNotification();
    checkEnvironmentalAlarms(); // Çevresel kontrol eklendi
    updateOLED();
  }
}

void readSensors() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h)) humidity    = h;
  if (!isnan(t)) temperature = t;

  int raw    = analogRead(SOIL_PIN);
  int mapped = map(raw, 4095, 1500, 0, 100);
  mapped     = constrain(mapped, 0, 100);
  moisture   = getSmoothedMoisture((float)mapped);
}

void runControlLogic() {
  if (!isAutoMode) {
    localPumpDesired = pumpStatus;
    return;
  }

  float mMin    = activeProfile.moistureMin;
  float mTarget = activeProfile.moistureTarget;
  float mMax    = activeProfile.moistureMax;

  bool newDesired = false;

  if (moisture >= mMax) newDesired = false;
  else if (moisture <= mMin) newDesired = true;
  else if (moisture < mTarget) newDesired = true;
  else {
    if (localPumpDesired) newDesired = false;
    else newDesired = (temperature > activeProfile.dangerTempC);
  }

  if (newDesired && !localPumpDesired) {
    pumpStartTime    = millis();
    localPumpDesired = true;
  }
  else if (!newDesired && localPumpDesired) {
    if (millis() - pumpStartTime >= MIN_PUMP_MS) localPumpDesired = false;
  }
  else localPumpDesired = newDesired;

  if (pumpStatus != localPumpDesired) pumpStatus = localPumpDesired;
}

void applyRelay() {
  digitalWrite(RELAY_PIN, pumpStatus ? LOW : HIGH);
}

void checkPushNotification() {
  if (pumpStatus == prevPumpStatus) return;
  
  char msg[64];
  if (pumpStatus && !prevPumpStatus) {
    snprintf(msg, sizeof(msg), "Sulama BASLADI | Toprak: %d%%", (int)moisture);
    cloudMessage = String(msg);
  } else if (!pumpStatus && prevPumpStatus) {
    snprintf(msg, sizeof(msg), "Sulama BITTI | Toprak: %d%%", (int)moisture);
    cloudMessage = String(msg);
  }
  prevPumpStatus = pumpStatus;
}

// Yeni: Çevresel Takip Bildirimleri
void checkEnvironmentalAlarms() {
  static unsigned long lastAlarmTime = 0;
  // Günde çok fazla mesaj gelmemesi için 1 saatte bir alarm ver (3600000 ms)
  if (millis() - lastAlarmTime < 3600000 && lastAlarmTime != 0) return; 

  char msg[64];
  bool alarmSent = false;

  if (temperature > activeProfile.dangerTempC) {
    snprintf(msg, sizeof(msg), "SICAKLIK ALARMI: %.1fC - Bitki tehlikede!", temperature);
    cloudMessage = String(msg);
    alarmSent = true;
  }
  else if (humidity < activeProfile.stressHumidity) {
    snprintf(msg, sizeof(msg), "NEM DUSUK: %.1f%% - Bitki strese girebilir.", humidity);
    cloudMessage = String(msg);
    alarmSent = true;
  }

  if (alarmSent) {
    lastAlarmTime = millis();
    Serial.print("[Alarm Sent] ");
    Serial.println(msg);
  }
}

// ============================================================
//  OLED GÖRÜNÜM
// ============================================================
void drawOledStatic() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 0);
  display.print("AGROCLOUD MONITOR");
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  display.setCursor(0, 13);  display.print("Temp:");
  display.setCursor(65, 13); display.print("Hum:");
  display.setCursor(0, 23);  display.print("Soil:");
  display.drawLine(0, 32, 127, 32, SSD1306_WHITE);
  display.setCursor(0, 36);  display.print("Mode:");
  display.setCursor(65, 36); display.print("Pump:");
  display.setCursor(0, 46);  display.print("Thr:");
  display.drawLine(0, 55, 127, 55, SSD1306_WHITE);
  display.setCursor(0, 57);  display.print("IP:");
  display.setCursor(45, 57); display.print("RSSI:");
  display.display();
  oledInitDone = true;
}

void oledUpdateField(int x, int y, int clearW, const char* newVal) {
  display.fillRect(x, y, clearW, 8, SSD1306_BLACK);
  display.setCursor(x, y);
  display.print(newVal);
}

void updateOLED() {
  if (!oledInitDone) drawOledStatic();
  char buf[16];
  bool changed = false;

  if ((int)temperature != oledCache.temp) {
    snprintf(buf, sizeof(buf), "%dC", (int)temperature);
    oledUpdateField(30, 13, 30, buf);
    oledCache.temp = (int)temperature;
    changed = true;
  }
  if ((int)humidity != oledCache.hum) {
    snprintf(buf, sizeof(buf), "%d%%", (int)humidity);
    oledUpdateField(90, 13, 35, buf);
    oledCache.hum = (int)humidity;
    changed = true;
  }
  if ((int)moisture != oledCache.soil) {
    snprintf(buf, sizeof(buf), "%d%%", (int)moisture);
    oledUpdateField(30, 23, 40, buf);
    oledCache.soil = (int)moisture;
    changed = true;
  }
  if (isAutoMode != oledCache.autoMode) {
    oledUpdateField(30, 36, 30, isAutoMode ? "AUTO" : "MANU");
    oledCache.autoMode = isAutoMode;
    changed = true;
  }
  if (pumpStatus != oledCache.pump) {
    oledUpdateField(95, 36, 30, pumpStatus ? "ON" : "OFF");
    oledCache.pump = pumpStatus;
    changed = true;
  }
  if ((int)moistureThreshold != oledCache.threshold) {
    snprintf(buf, sizeof(buf), "%d%%", (int)moistureThreshold);
    oledUpdateField(28, 46, 30, buf);
    oledCache.threshold = (int)moistureThreshold;
    changed = true;
  }
  
  // Bitki Adı + ID Gösterimi
  char plantBuf[16];
  snprintf(plantBuf, sizeof(plantBuf), "[%d]%s", plantPreset, activeProfile.name);
  if (strcmp(plantBuf, oledCache.plant) != 0) {
    display.fillRect(60, 46, 68, 8, SSD1306_BLACK);
    display.setCursor(60, 46);
    display.print(plantBuf);
    strncpy(oledCache.plant, plantBuf, 14);
    changed = true;
  }

  int curRSSI  = WiFi.RSSI();
  bool curConn = ArduinoCloud.connected();
  if (curRSSI != oledCache.rssi || curConn != oledCache.iotConn) {
    display.fillRect(18, 57, 25, 8, SSD1306_BLACK);
    display.setCursor(18, 57);
    String ip = WiFi.localIP().toString();
    display.print(ip.substring(ip.lastIndexOf('.') + 1));
    snprintf(buf, sizeof(buf), "%d", curRSSI);
    oledUpdateField(72, 57, 20, buf);
    display.fillRect(95, 57, 30, 8, SSD1306_BLACK);
    display.setCursor(95, 57);
    display.print(curConn ? "IOT" : "ERR");
    oledCache.rssi = curRSSI;
    oledCache.iotConn = curConn;
    changed = true;
  }
  if (changed) display.display();
}

// ============================================================
//  CLOUD CALLBACKS
// ============================================================
void onMoistureThresholdChange() {
  if (millis() - lastPresetChangeTime < SYNC_GUARD_MS) return;
  int val = (int)moistureThreshold;
  if (val <= 0 || val == activeProfile.moistureTarget) return;

  if (plantPreset != 0) {
    plantPreset = 0;
    activeProfile = PLANTS[0];
    activeProfile.moistureTarget = val;
    cloudMessage = "Manuel: " + String(val);
  } else {
    activeProfile.moistureTarget = val;
  }
}

void onPlantPresetChange() {
  lastPresetChangeTime = millis();
  if (plantPreset < 0 || plantPreset > 3) plantPreset = 0;
  activeProfile = PLANTS[plantPreset];
  moistureThreshold = activeProfile.moistureTarget;
  cloudMessage = "Mod: " + String(activeProfile.name);
}

void onPumpStatusChange() {
  if (!isAutoMode) applyRelay();
}

void onIsAutoModeChange() {}
void onCloudMessageChange() {}
