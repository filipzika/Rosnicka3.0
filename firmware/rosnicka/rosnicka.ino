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

// Doba spanku mezi merenimi (v sekundach).
// ESP8266 zvladne max ~3-4 hodiny na jeden spanek.
#define SLEEP_SECONDS   900UL   // 15 minut

// Timeout pro pripojeni k ulozene WiFi (s). Kdyz se nepripoji,
// jde rovnou spat a zkusi to znovu po probuzeni (setri baterii).
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

// ── Kontrola prikazu "reset" ze Serial monitoru ───────────────
// Po probuzeni cea SERIAL_WINDOW_MS na prikaz "reset" - kdyz prijde,
// smaze ulozene WiFi udaje a restartuje (spusti se captive portal).
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
  // WAKE_RF_DEFAULT = po probuzeni se znovu zapne WiFi
  ESP.deepSleep(SLEEP_SECONDS * 1000000ULL, WAKE_RF_DEFAULT);
  delay(100); // pojistka nez ESP skutecne usne
}

// ── Pripojeni k MQTT (kratky pokus, jinak spat) ───────────────
bool connectMQTT() {
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  String clientId = "rosnicka-" + String(ESP.getChipId(), HEX);

  for (uint8_t i = 0; i < 3; i++) {
    Serial.print("MQTT connect... ");
    if (mqtt.connect(clientId.c_str())) {
      Serial.println("OK");
      return true;
    }
    Serial.print("chyba rc=");
    Serial.println(mqtt.state());
    delay(1000);
  }
  return false;
}

void setup() {
  Serial.begin(74880);
  delay(50);
  Serial.println();
  Serial.println("=== Rosnicka 3.0 (probuzeni) ===");

  // ── Okno pro prikaz "reset" ─────────────────────────────────
  checkSerialReset();

  // ── Senzor ──────────────────────────────────────────────────
  Wire.begin(SDA_PIN, SCL_PIN);
  if (!aht.begin()) {
    Serial.println("AHT20 nenalezen! Spim a zkusim pozdeji.");
    goToSleep();
    return;
  }

  // Prumer z SAMPLE_COUNT mereni (potlaci sum senzoru)
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

  if (valid == 0) {
    Serial.println("Zadny platny vzorek. Spim.");
    goToSleep();
    return;
  }

  float t = sumT / valid;
  float h = sumH / valid;

  Serial.print("PRUMER ("); Serial.print(valid); Serial.print("/");
  Serial.print(SAMPLE_COUNT); Serial.print("): ");
  Serial.print(t, 1); Serial.print(" C  ");
  Serial.print(h, 1); Serial.println(" %");

  // ── WiFi (WiFiManager + captive portal) ─────────────────────
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);   // portal se sam zavre po 3 min
  wm.setConnectTimeout(WIFI_TIMEOUT_S);

  // Captive portal: presmeruje vsechny http dotazy na config stranku,
  // takze "vyskoci" sam po pripojeni k AP (jako WiFi v kavarne).
  wm.setCaptivePortalEnable(true);
  wm.setShowInfoUpdate(false);
  wm.setTitle("Rosnicka 3.0");

  // Skryje branding "WiFiManager" / verzi v paticce portalu
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
    // Pripojeni selhalo (nebo vyprsel portal) - spat a zkusit znovu.
    Serial.println("WiFi nepripojeno. Spim a zkusim po probuzeni.");
    goToSleep();
    return;
  }

  Serial.print("WiFi OK, IP: ");
  Serial.println(WiFi.localIP());

  // ── NTP cas ─────────────────────────────────────────────────
  ntp.begin();
  unsigned long ts = 0;
  for (uint8_t i = 0; i < 10; i++) {
    if (ntp.forceUpdate()) { ts = ntp.getEpochTime(); break; }
    delay(300);
  }
  Serial.print("NTP cas (epoch): ");
  Serial.println(ts);

  // ── MQTT publish ────────────────────────────────────────────
  if (connectMQTT()) {
    char payload[128];
    snprintf(payload, sizeof(payload),
      "{\"id\":\"%s\",\"t\":%.1f,\"h\":%.1f,\"ts\":%lu}",
      DEVICE_ID, t, h, ts
    );

    // retained=true: broker ulozi posledni hodnotu pro nove klienty
    if (mqtt.publish(MQTT_TOPIC, payload, true)) {
      Serial.println("MQTT odeslano OK");
    } else {
      Serial.println("MQTT publish selhal");
    }

    mqtt.loop();
    delay(100);          // necha dokoncit odeslani
    mqtt.disconnect();   // ciste odpojeni
  } else {
    Serial.println("MQTT nepripojeno, data zahozena.");
  }

  // ── Spanek ──────────────────────────────────────────────────
  goToSleep();
}

void loop() {
  // Nikdy se nespusti - po deep sleep ESP startuje vzdy od setup().
}
