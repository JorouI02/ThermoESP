#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "max6675.h"

// LCD
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
LiquidCrystal_I2C lcd(0x27, 20, 4);

// Файлова система за CSV
#include <FS.h>
#include <LittleFS.h>



// NTP за дата и час
#include <time.h>
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3 * 3600;   // GMT+3
const int daylightOffset_sec = 0;

// ---- Wi-Fi ----
const char* ssid = "Metodievi";
const char* pass = "ilia940311";

// ---- HiveMQ Cloud ----
const char* mqtt_host  = "8dfb5d8ea05e42479e6203aca4957355.s1.eu.hivemq.cloud";
const int   mqtt_port  = 8883;
const char* mqtt_user  = "ThermoESP";
const char* mqtt_passw = "Thermo123";
const char* mqtt_topic = "esp32/thermo/values";

// ---- MAX6675 (пример, попълни 2–6) ----
const int SCK1_PIN = 13;
const int SO1_PIN  = 14;
const int CS1_PIN  = 12;
const int SCK2_PIN = 13; const int SO2_PIN = 14; const int CS2_PIN = 27;
const int SCK3_PIN = 13; const int SO3_PIN = 14; const int CS3_PIN = 26;
const int SCK4_PIN = 17; const int SO4_PIN = 19; const int CS4_PIN = 18;
const int SCK5_PIN = 17; const int SO5_PIN = 19; const int CS5_PIN = 5;
const int SCK6_PIN = 17; const int SO6_PIN = 35; const int CS6_PIN = 33;

MAX6675 *T[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };

WiFiClientSecure net;
PubSubClient mqtt(net);

unsigned long lastMqtt = 0;
unsigned long lastCsv  = 0;
const unsigned long mqttEveryMs = 1000;
const unsigned long csvEveryMs  = 30000;
uint32_t seq = 0;

void lcdPrintLine(uint8_t row, const String &text) {
  lcd.setCursor(0, row);
  String s = text;
  while (s.length() < 20) s += ' ';
  if (s.length() > 20) s = s.substring(0, 20);
  lcd.print(s);
}

void mqttReconnect() {
  while (!mqtt.connected()) {
    String cid = "esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqtt.connect(cid.c_str(), mqtt_user, mqtt_passw)) break;
    delay(1000);
  }
}

bool initOneMAX(int idx, int sck, int cs, int so) {
  if (sck <= 0 || cs <= 0 || so <= 0) return false;
  T[idx] = new MAX6675(sck, cs, so);
  return true;
}

void ensureCsvHeader() {
  if (!LittleFS.exists("/thermo_log.csv")) {
    File f = LittleFS.open("/thermo_log.csv", FILE_WRITE);
    if (f) {
      f.println("datetime,T1,T2,T3,T4,T5,T6");
      f.close();
    }
  }
}

void appendCsv(float v[6]) {
  File f = LittleFS.open("/thermo_log.csv", FILE_APPEND);
  if (!f) return;

  String dt = getDateTime(); // вече имаш тази функция
  auto fOrBlank = [](float x, char *buf, size_t n){
    if (isnan(x)) snprintf(buf, n, "");
    else snprintf(buf, n, "%.2f", x);
  };
  char b1[16],b2[16],b3[16],b4[16],b5[16],b6[16];
  fOrBlank(v[0], b1, sizeof(b1));
  fOrBlank(v[1], b2, sizeof(b2));
  fOrBlank(v[2], b3, sizeof(b3));
  fOrBlank(v[3], b4, sizeof(b4));
  fOrBlank(v[4], b5, sizeof(b5));
  fOrBlank(v[5], b6, sizeof(b6));

  char line[256];
  snprintf(line, sizeof(line), "%s,%s,%s,%s,%s,%s,%s",
           dt.c_str(), b1,b2,b3,b4,b5,b6);
  f.println(line);
  f.close();
}

// Праг за "промяна" в °C
static const float CHANGE_EPS = 0.10f;
static float lastVals[6] = {NAN,NAN,NAN,NAN,NAN,NAN};

void appendPretty(float v[6]) {
  File f = LittleFS.open("/thermo_pretty.log", FILE_APPEND);
  if (!f) return;

  String dt = getDateTime();

  // Форматиране: фиксирана ширина, повече разстояние, * ако има промяна
  auto cell = [&](int idx, char* out, size_t n){
    if (isnan(v[idx])) {
      snprintf(out, n, "T%d:   --   ", idx+1);
    } else {
      bool changed = isnan(lastVals[idx]) || fabsf(v[idx]-lastVals[idx]) >= CHANGE_EPS;
      // Пример: "T1:  23.50  " или "T1:  23.50* "
      snprintf(out, n, "T%d:%7.2f%s  ", idx+1, v[idx], changed ? "*" : " ");
    }
  };

  char c1[16],c2[16],c3[16],c4[16],c5[16],c6[16];
  cell(0,c1,sizeof(c1)); cell(1,c2,sizeof(c2)); cell(2,c3,sizeof(c3));
  cell(3,c4,sizeof(c4)); cell(4,c5,sizeof(c5)); cell(5,c6,sizeof(c6));

  char line[200];
  snprintf(line, sizeof(line), "%s   %s %s %s %s %s %s",
           dt.c_str(), c1,c2,c3,c4,c5,c6);
  f.println(line);
  f.close();

  // Обнови последните стойности
  for (int i=0;i<6;i++) lastVals[i] = v[i];
}

String getDateTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "N/A";
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

void setupSdMirror();
void loopSdMirror();

void setup() {
  Serial.begin(115200);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcdPrintLine(0, "Booting...");

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  } else {
    ensureCsvHeader();
  }

  initOneMAX(0, SCK1_PIN, CS1_PIN, SO1_PIN);
  initOneMAX(1, SCK2_PIN, CS2_PIN, SO2_PIN);
  initOneMAX(2, SCK3_PIN, CS3_PIN, SO3_PIN);
  initOneMAX(3, SCK4_PIN, CS4_PIN, SO4_PIN);
  initOneMAX(4, SCK5_PIN, CS5_PIN, SO5_PIN);
  initOneMAX(5, SCK6_PIN, CS6_PIN, SO6_PIN);

  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println("\nWiFi OK");

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  net.setInsecure();
  mqtt.setServer(mqtt_host, mqtt_port);
  mqtt.setKeepAlive(60);
  mqttReconnect();
  Serial.println("MQTT OK");
  
  setupSdMirror();   // в setup()

}

void loop() {
  if (!mqtt.connected()) mqttReconnect();
  mqtt.loop();

  float vals[6];
  for (int i=0;i<6;i++) {
    if (T[i]) vals[i] = T[i]->readCelsius();
    else vals[i] = NAN;
  }

  char l0[21]; snprintf(l0, sizeof(l0), "T1:%5.1fC T2:%5.1fC",
                        isnan(vals[0])?0.0:vals[0], isnan(vals[1])?0.0:vals[1]);
  lcdPrintLine(0, String(l0));
  char l1[21]; snprintf(l1, sizeof(l1), "T3:%5.1fC T4:%5.1fC",
                        isnan(vals[2])?0.0:vals[2], isnan(vals[3])?0.0:vals[3]);
  lcdPrintLine(1, String(l1));
  char l2[21]; snprintf(l2, sizeof(l2), "T5:%5.1fC T6:%5.1fC",
                        isnan(vals[4])?0.0:vals[4], isnan(vals[5])?0.0:vals[5]);
  lcdPrintLine(2, String(l2));
  lcdPrintLine(3, "");

  if (millis() - lastMqtt >= mqttEveryMs) {
    lastMqtt = millis();
    String json = "{";
    json += "\"ts\":"; json += millis(); json += ",";
    json += "\"seq\":"; json += seq++;
    json += ",\"datetime\":\""; json += getDateTime(); json += "\"";
    auto addField = [&](const char* name, float v){
      if (!isnan(v)) { json += ","; json += "\""; json += name; json += "\":"; json += String(v,2); }
    };
    addField("T1", vals[0]);
    addField("T2", vals[1]);
    addField("T3", vals[2]);
    addField("T4", vals[3]);
    addField("T5", vals[4]);
    addField("T6", vals[5]);
    json += "}";
    mqtt.publish(mqtt_topic, json.c_str(), true);
    // Serial.println(json);
  }

  if (millis() - lastCsv >= csvEveryMs) {
    lastCsv = millis();
    appendCsv(vals);
    appendPretty(vals);

  }
loopSdMirror();    // в loop(), някъде в края

  delay(100);
}
