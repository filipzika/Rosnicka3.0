/*
 * Rosnicka 3.0 - ESP-12F (ESP8266) + AHT20
 *
 * Potrebne knihovny (Library Manager):
 *   - Adafruit AHTX0
 *   - WiFiManager (by tzapu)
 *   - PubSubClient (by Nick O'Leary)
 */

#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>

// ===== Konfigurace =====
#define DEVICE_ID       "rosnicka_01"
#define MQTT_BROKER     "broker.emqx.io"
#define MQTT_PORT       1883
#define MQTT_TOPIC      "rosnicka/" DEVICE_ID "/sensors"
#define SEND_INTERVAL   60000UL

// I2C piny na ESP-12F: SDA=GPIO4(D2), SCL=GPIO5(D1)
#define SDA_PIN 4
#define SCL_PIN 5

Adafruit_AHTX0 aht;
WiFiClient     wifiClient;
PubSubClient   mqtt(wifiClient);

unsigned long lastSend = 0;

void reconnectMQTT() {
  while (!mqtt.connected()) {
    String clientId = "rosnicka-" + String(ESP.getChipId(), HEX);
    Serial.print("MQTT connect... ");
    if (mqtt.connect(clientId.c_str())) {
      Serial.println("OK");
    } else {
      Serial.print("chyba rc=");
      Serial.print(mqtt.state());
      Serial.println(", zkusim za 5s");
      delay(5000);
    }
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.begin(74880);
  delay(100);
  Serial.println();
  Serial.println("=== Rosnicka 3.0 ===");

  Wire.begin(SDA_PIN, SCL_PIN);
  if (!aht.begin()) {
    Serial.println("AHT20 nenalezen! Zkontroluj zapojeni I2C.");
    while (1) delay(10);
  }
  Serial.println("AHT20 OK");

  // WiFiManager - pri prvnim spusteni vytvori AP "Rosnicka-3.0"
  // Pripoj se na nej a nastav WiFi pres webovy portal
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setAPCallback([](WiFiManager *) {
    Serial.println("Konfiguracni portal: SSID=Rosnicka-3.0");
    Serial.println("Otevri 192.168.4.1");
  });

  if (!wm.autoConnect("Rosnicka-3.0")) {
    Serial.println("WiFi konfigurace selhala, restartuji...");
    ESP.restart();
  }

  Serial.print("WiFi pripojeno, IP: ");
  Serial.println(WiFi.localIP());
  digitalWrite(LED_BUILTIN, LOW);

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setKeepAlive(60);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi ztraceno, restartuji...");
    delay(1000);
    ESP.restart();
  }

  if (!mqtt.connected()) reconnectMQTT();
  mqtt.loop();

  unsigned long now = millis();
  if (now - lastSend >= SEND_INTERVAL) {
    lastSend = now;

    sensors_event_t humidity, temp;
    aht.getEvent(&humidity, &temp);

    float t = temp.temperature;
    float h = humidity.relative_humidity;

    if (isnan(t) || isnan(h)) {
      Serial.println("Chyba cteni senzoru");
      return;
    }

    Serial.print("T: ");
    Serial.print(t, 1);
    Serial.print(" C  H: ");
    Serial.print(h, 1);
    Serial.println(" %");

    char payload[96];
    snprintf(payload, sizeof(payload),
      "{\"id\":\"%s\",\"t\":%.1f,\"h\":%.1f}",
      DEVICE_ID, t, h
    );

    // retained=true: broker ulozi posledni hodnotu pro nove klienty
    if (mqtt.publish(MQTT_TOPIC, payload, true)) {
      Serial.println("MQTT OK");
      digitalWrite(LED_BUILTIN, HIGH);
      delay(50);
      digitalWrite(LED_BUILTIN, LOW);
    } else {
      Serial.println("MQTT chyba odeslani");
    }
  }
}
