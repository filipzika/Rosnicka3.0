/*
 * Rosnicka 3.0 - ESP-12F (ESP8266) + AHT20
 *
 * Potrebne knihovny (Library Manager):
 *   - Adafruit AHTX0
 *   - WiFiManager (by tzapu)
 *   - PubSubClient (by Nick O'Leary)
 *   - NTPClient (by Fabrice Weinberg)
 *
 * ┌─────────────────────────────────────────────────────────────┐
 * │  DEEP SLEEP - HARDWAROVY PREDPOKLAD                          │
 * │  Pro probuzeni z deep sleep MUSI byt propojeny piny:         │
 * │       GPIO16 (D0)  ──────  RST                               │
 * │  Bez tohoto dratu se ESP po uspani uz neprobudi!             │
 * │  Pri nahravani firmware muze tento drat vadit - pokud        │
 * │  nejde nahrat sketch, docasne ho odpoj.                      │
 * └─────────────────────────────────────────────────────────────┘
 *
 * ═══ REZIMY ═══
 *   DEEP SLEEP   - ESP zmeri, odesle a usne na SLEEP_SECONDS (uspora baterie)
 *   KONTINUALNI  - ESP je stale pripojene a odesila kazdych
 *                  CONTINUOUS_INTERVAL_MS (15 s), bez deep sleep
 *
 *   Vychozi rezim urcuje DEFAULT_DEEP_SLEEP nize.
 *   Rezim lze prepnout z webu (retained zprava na topicu .../config).
 *   Pozn.: v deep sleep rezimu se zmena z webu projevi az pri pristim
 *   probuzeni (ESP behem spanku neposloucha).
 */

#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <WiFiUDP.h>
#include <NTPClient.h>
#include <PubSubClient.h>

// ===== Konfigurace =====
#define DEVICE_ID       "rosnicka_01"
#define MQTT_BROKER     "broker.emqx.io"
#define MQTT_PORT       1883
#define MQTT_TOPIC      "rosnicka/" DEVICE_ID "/sensors"
#define CONFIG_TOPIC    "rosnicka/" DEVICE_ID "/config"
#define CMD_TOPIC       "rosnicka/" DEVICE_ID "/cmd"

// ── REZIM ─────────────────────────────────────────────────────
// true  = deep sleep (uspora baterie)
// false = kontinualni (odesila kazdych 15 s, bez spanku)
// Toto je VYCHOZI rezim; web ho muze prepsat retained config zpravou.
#define DEFAULT_DEEP_SLEEP   true

// Deep sleep: doba spanku mezi merenimi (s). Max ~3-4 h na jeden spanek.
#define SLEEP_SECONDS        900UL    // 15 minut

// Kontinualni: interval odesilani (ms)
#define CONTINUOUS_INTERVAL_MS 15000UL // 15 s

// Timeout pro pripojeni k ulozene WiFi (s)
#define WIFI_TIMEOUT_S  15

// Pocet mereni, ze kterych se spocita prumer
#define SAMPLE_COUNT    5
#define SAMPLE_DELAY_MS 300

// I2C piny na ESP-12F: SDA=GPIO4(D2), SCL=GPIO5(D1)
#define SDA_PIN 4
#define SCL_PIN 5

Adafruit_AHTX0 aht;
WiFiClient     wifiClient;
PubSubClient   mqtt(wifiClient);

WiFiUDP   ntpUDP;
NTPClient ntp(ntpUDP, "pool.ntp.org", 0, 60000); // UTC

// ── Globalni stav ─────────────────────────────────────────────
bool deepSleepMode    = DEFAULT_DEEP_SLEEP; // aktualni rezim
bool configReceived   = false;              // dorazila config zprava?
unsigned long lastSend = 0;                 // pro kontinualni rezim
String pendingCmd     = "";                 // prikaz z webu k provedeni

// ── Reset WiFi pres Serial monitor ────────────────────────────
// Funguje bez internetu - staci USB-serial adapter (ten samy, kterym
// nahravas firmware). Napis "reset" do 3s okna po startu.
// Pozn.: Hlavni offline cesta pro prenastaveni site je captive portal,
//        ktery se sam otevre kdyz se ESP nepripoji k ulozene WiFi
//        (pripoj se mobilem na AP "Rosnicka-3.0", bez internetu).
#define SERIAL_WINDOW_MS 3000
void checkSerialReset() {
  Serial.println("Napis 'reset' do 3s pro vymazani WiFi nastaveni...");
  String cmd = "";
  unsigned long start = millis();
  while (millis() - start < SERIAL_WINDOW_MS) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        cmd.trim();
        if (cmd.equalsIgnoreCase("reset")) {
          Serial.println(">> Mazu WiFi nastaveni...");
          WiFiManager wm;
          wm.resetSettings();
          Serial.println(">> Hotovo, restartuji do config portalu.");
          delay(500);
          ESP.restart();
        }
        cmd = "";
      } else {
        cmd += c;
      }
    }
    delay(10);
  }
}

// ── Uspani ────────────────────────────────────────────────────
void goToSleep() {
  Serial.print("Uspavam na ");
  Serial.print(SLEEP_SECONDS);
  Serial.println(" s...");
  Serial.flush();
  ESP.deepSleep(SLEEP_SECONDS * 1000000ULL, WAKE_RF_DEFAULT);
  delay(100);
}

// ── MQTT callback - cte rezim a prikazy z webu ────────────────
void onMqttMessage(char *topic, byte *payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  String t(topic);

  if (t.endsWith("/config")) {
    // payload napr. {"deepsleep":true} nebo {"deepsleep":false}
    if (msg.indexOf("false") >= 0)      deepSleepMode = false;
    else if (msg.indexOf("true") >= 0)  deepSleepMode = true;
    configReceived = true;
    Serial.print("Config z webu: deepSleep=");
    Serial.println(deepSleepMode ? "true" : "false");
  }
  else if (t.endsWith("/cmd")) {
    if (len == 0) return;  // prazdna (smazana) retained zprava
    if (msg.indexOf("reset_wifi") >= 0)    pendingCmd = "reset_wifi";
    else if (msg.indexOf("restart") >= 0)  pendingCmd = "restart";
    Serial.print("Prikaz z webu: ");
    Serial.println(pendingCmd);
  }
}

// ── Provede cekajici prikaz z webu ────────────────────────────
void handlePendingCommand() {
  if (pendingCmd == "") return;
  String c = pendingCmd;
  pendingCmd = "";

  // Smaze retained prikaz, aby se po restartu znovu nevykonal (zacykleni!)
  mqtt.publish(CMD_TOPIC, "", true);
  mqtt.loop();
  delay(150);

  if (c == "restart") {
    Serial.println(">> RESTART na pokyn z webu");
    mqtt.disconnect();
    delay(100);
    ESP.restart();
  }
  else if (c == "reset_wifi") {
    Serial.println(">> RESET WIFI na pokyn z webu");
    mqtt.disconnect();
    WiFiManager wm;
    wm.resetSettings();
    delay(200);
    ESP.restart();   // po restartu nabehne captive portal
  }
}

// ── Pripojeni k MQTT + odber config topicu ────────────────────
bool connectMQTT() {
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  String clientId = "rosnicka-" + String(ESP.getChipId(), HEX);

  for (uint8_t i = 0; i < 3; i++) {
    Serial.print("MQTT connect... ");
    if (mqtt.connect(clientId.c_str())) {
      Serial.println("OK");
      mqtt.subscribe(CONFIG_TOPIC);
      mqtt.subscribe(CMD_TOPIC);
      return true;
    }
    Serial.print("chyba rc=");
    Serial.println(mqtt.state());
    delay(1000);
  }
  return false;
}

// ── Cte SAMPLE_COUNT mereni a vrati prumer ────────────────────
bool readAverage(float &t, float &h) {
  float sumT = 0, sumH = 0;
  uint8_t valid = 0;
  for (uint8_t i = 0; i < SAMPLE_COUNT; i++) {
    sensors_event_t humidity, temp;
    aht.getEvent(&humidity, &temp);
    if (isnan(temp.temperature) || isnan(humidity.relative_humidity)) {
      Serial.print("vzorek "); Serial.print(i + 1); Serial.println(": chyba");
    } else {
      sumT += temp.temperature;
      sumH += humidity.relative_humidity;
      valid++;
      Serial.print("vzorek "); Serial.print(i + 1); Serial.print(": ");
      Serial.print(temp.temperature, 1); Serial.print(" C  ");
      Serial.print(humidity.relative_humidity, 1); Serial.println(" %");
    }
    if (i < SAMPLE_COUNT - 1) delay(SAMPLE_DELAY_MS);
  }
  if (valid == 0) return false;
  t = sumT / valid;
  h = sumH / valid;
  Serial.print("PRUMER ("); Serial.print(valid); Serial.print("/");
  Serial.print(SAMPLE_COUNT); Serial.print("): ");
  Serial.print(t, 1); Serial.print(" C  ");
  Serial.print(h, 1); Serial.println(" %");
  return true;
}

// ── Zmeri prumer a odesle na MQTT ─────────────────────────────
void measureAndPublish() {
  float t, h;
  if (!readAverage(t, h)) {
    Serial.println("Zadny platny vzorek, preskakuji.");
    return;
  }

  ntp.update();
  unsigned long ts = ntp.getEpochTime();

  char payload[128];
  snprintf(payload, sizeof(payload),
    "{\"id\":\"%s\",\"t\":%.1f,\"h\":%.1f,\"ts\":%lu}",
    DEVICE_ID, t, h, ts
  );

  if (mqtt.publish(MQTT_TOPIC, payload, true)) { // retained
    Serial.println("MQTT odeslano OK");
  } else {
    Serial.println("MQTT publish selhal");
  }
  mqtt.loop();
}

void setup() {
  Serial.begin(74880);
  delay(50);
  Serial.println();
  Serial.println("=== Rosnicka 3.0 ===");

  checkSerialReset();

  // ── Senzor ──────────────────────────────────────────────────
  Wire.begin(SDA_PIN, SCL_PIN);
  if (!aht.begin()) {
    Serial.println("AHT20 nenalezen!");
    if (DEFAULT_DEEP_SLEEP) goToSleep();
    else { delay(5000); ESP.restart(); }
    return;
  }

  // ── WiFi (WiFiManager + captive portal) ─────────────────────
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(WIFI_TIMEOUT_S);
  wm.setCaptivePortalEnable(true);
  wm.setShowInfoUpdate(false);
  wm.setTitle("Rosnicka 3.0");
  wm.setCustomHeadElement(
    "<style>"
    ".wrap > div:last-child,"
    "div[style*='text-align:right'],"
    "a[href*='WiFiManager']"
    "{display:none!important;}"
    "</style>"
  );
  wm.setAPStaticIPConfig(
    IPAddress(192, 168, 4, 1),
    IPAddress(192, 168, 4, 1),
    IPAddress(255, 255, 255, 0)
  );
  wm.setAPCallback([](WiFiManager *) {
    Serial.println("== Captive portal aktivni ==");
    Serial.println("Pripoj se na WiFi: Rosnicka-3.0");
    Serial.println("Portal vyskoci sam, nebo otevri http://192.168.4.1");
  });

  if (!wm.autoConnect("Rosnicka-3.0")) {
    Serial.println("WiFi nepripojeno.");
    if (DEFAULT_DEEP_SLEEP) goToSleep();
    else { delay(2000); ESP.restart(); }
    return;
  }
  Serial.print("WiFi OK, IP: ");
  Serial.println(WiFi.localIP());

  ntp.begin();
  ntp.forceUpdate();

  // ── MQTT + nacteni rezimu z webu ────────────────────────────
  if (!connectMQTT()) {
    Serial.println("MQTT nepripojeno.");
    if (DEFAULT_DEEP_SLEEP) goToSleep();
    else { delay(2000); ESP.restart(); }
    return;
  }

  // Pockej kratce na retained config/cmd zpravy z webu
  Serial.println("Ctu nastaveni z webu...");
  unsigned long w = millis();
  while (!configReceived && millis() - w < 1500) {
    mqtt.loop();
    delay(20);
  }

  // Pripadny cekajici prikaz (restart / reset wifi) provedeme hned
  handlePendingCommand();

  Serial.print("Aktivni rezim: ");
  Serial.println(deepSleepMode ? "DEEP SLEEP" : "KONTINUALNI");

  // ── Prvni mereni a odeslani ─────────────────────────────────
  measureAndPublish();
  lastSend = millis();

  // ── Rozhodnuti dle rezimu ───────────────────────────────────
  if (deepSleepMode) {
    delay(100);
    mqtt.disconnect();
    goToSleep();              // loop() se uz nespusti
  }
  // Kontinualni rezim - pokracuje v loop()
}

void loop() {
  // Spousti se jen v KONTINUALNIM rezimu.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi ztraceno, restartuji...");
    delay(1000);
    ESP.restart();
  }

  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();   // prijima zmenu rezimu / prikazy z webu

  // Prikaz z webu (restart / reset wifi)
  handlePendingCommand();

  // Web prepnul na deep sleep -> odesli a usni
  if (deepSleepMode) {
    Serial.println("Web prepnul na DEEP SLEEP.");
    measureAndPublish();
    delay(100);
    mqtt.disconnect();
    goToSleep();
  }

  // Periodicke odesilani
  if (millis() - lastSend >= CONTINUOUS_INTERVAL_MS) {
    lastSend = millis();
    measureAndPublish();
  }

  ntp.update();
  delay(50);
}
