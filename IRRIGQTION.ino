#include <WiFi.h>
#include <PubSubClient.h>

// =================================
// 1. CONNEXION WIFI
// =================================

const char* WIFI_SSID = "Galaxy A06 0504";
const char* WIFI_PASSWORD = "cycy12@&";

// =================================
// 2. CONFIGURATION THINGSBOARD
// =================================

const char* TB_SERVER = "thingsboard.cloud";
const int TB_PORT = 1883;
const char* TB_TOKEN = "r5ricPBwL418vyO6rkUk";

// =================================
// 3. BROCHES ESP32
// =================================

#define SOIL_PIN 34
#define LDR_PIN 35

#define RELAY_PUMP 27
#define RELAY_LAMP 26

// =================================
// 4. INITIALISATION
// =================================

WiFiClient espClient;
PubSubClient client(espClient);

// Relais actifs a LOW
#define RELAY_ON LOW
#define RELAY_OFF HIGH

// Calibration humidite du sol (a regler avec tes mesures)
const int SOIL_DRY = 3000;
const int SOIL_WET = 1200;

// Seuils sol (hysteresis)
const int SOIL_START = 35;   // pompe ON si en dessous
const int SOIL_STOP = 45;    // pompe OFF si au dessus

// LDR : 3.3V --- LDR ---+--- GPIO35
//                       |
//                     10 kOhm
//                       |
//                      GND
// Plus il y a de lumiere, plus la valeur est GRANDE (0 a 4095).
// A CALIBRER avec tes mesures.
const int LIGHT_DARK = 800;    // lampe ON si la valeur passe sous ce seuil
const int LIGHT_BRIGHT = 1200; // lampe OFF si la valeur depasse ce seuil

// Etats
bool pumpState = false;
bool lampState = false;
bool lampAuto = true;   // true = lampe commandee par la LDR, false = manuel

// Temporisations
unsigned long lastSensorRead = 0;
unsigned long lastSend = 0;
unsigned long lastMqttAttempt = 0;

const unsigned long SENSOR_INTERVAL = 2000;
const unsigned long SEND_INTERVAL = 5000;
const unsigned long MQTT_RETRY_INTERVAL = 5000;

// Valeurs des capteurs
int soilPercent = 0;
int lightValue = 0;     // valeur brute 0-4095
int lightPercent = 0;   // 0 = noir, 100 = tres lumineux

void sendToThingsBoard();

// =================================
// 5. CONNEXION WIFI
// =================================

void connectWiFi() {

  Serial.print("Connexion au WiFi");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connecte !");
  Serial.print("Adresse IP : ");
  Serial.println(WiFi.localIP());
}

// =================================
// 6a. COMMANDES A DISTANCE (RPC ThingsBoard)
// =================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {

  char msg[200];
  unsigned int n = length < sizeof(msg) - 1 ? length : sizeof(msg) - 1;
  memcpy(msg, payload, n);
  msg[n] = '\0';

  Serial.print("RPC recu : ");
  Serial.println(msg);

  // Identifiant de la requete = fin du topic
  const char* requestId = strrchr(topic, '/');
  if (requestId == NULL) {
    return;
  }
  requestId++;

  char responseTopic[100];
  snprintf(responseTopic, sizeof(responseTopic),
           "v1/devices/me/rpc/response/%s", requestId);

  bool paramTrue = (strstr(msg, "\"params\":true") != NULL) ||
                   (strstr(msg, "\"params\": true") != NULL) ||
                   (strstr(msg, "\"params\":\"true\"") != NULL) ||
                   (strstr(msg, "\"params\":1") != NULL);

  if (strstr(msg, "setLampAuto") != NULL) {

    // Retour au mode automatique (LDR) ou passage en manuel
    lampAuto = paramTrue;
    client.publish(responseTopic, lampAuto ? "true" : "false");

  } else if (strstr(msg, "setLamp") != NULL) {

    // Commande manuelle de la lampe
    lampAuto = false;
    lampState = paramTrue;
    digitalWrite(RELAY_LAMP, lampState ? RELAY_ON : RELAY_OFF);
    client.publish(responseTopic, lampState ? "true" : "false");

  } else if (strstr(msg, "getLampAuto") != NULL) {

    client.publish(responseTopic, lampAuto ? "true" : "false");

  } else if (strstr(msg, "getLamp") != NULL) {

    client.publish(responseTopic, lampState ? "true" : "false");
  }

  // Envoi immediat de l'etat mis a jour
  sendToThingsBoard();
}

// =================================
// 6b. CONNEXION THINGSBOARD (non bloquante)
// =================================

void tryReconnectMQTT() {

  unsigned long now = millis();

  if (now - lastMqttAttempt < MQTT_RETRY_INTERVAL) {
    return;
  }

  lastMqttAttempt = now;

  Serial.print("Connexion ThingsBoard...");

  if (client.connect("ESP32_Jardin", TB_TOKEN, "")) {
    Serial.println("Connecte !");
    client.subscribe("v1/devices/me/rpc/request/+");
  } else {
    Serial.print("Echec MQTT, code : ");
    Serial.println(client.state());
  }
}

// =================================
// 7. LECTURE DES CAPTEURS
// =================================

void readSensors() {

  // Humidite du sol
  int soilValue = analogRead(SOIL_PIN);

  soilPercent = map(soilValue, SOIL_DRY, SOIL_WET, 0, 100);
  soilPercent = constrain(soilPercent, 0, 100);

  // Luminosite LDR : moyenne de 10 mesures pour stabiliser
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(LDR_PIN);
    delay(2);
  }
  lightValue = sum / 10;

  lightPercent = map(lightValue, 0, 4095, 0, 100);
  lightPercent = constrain(lightPercent, 0, 100);

  // Affichage moniteur serie
  Serial.println("\n===== JARDIN INTELLIGENT =====");

  Serial.print("Humidite du sol : ");
  Serial.print(soilPercent);
  Serial.println(" %");

  Serial.print("Luminosite LDR : ");
  Serial.print(lightValue);
  Serial.print(" (");
  Serial.print(lightPercent);
  Serial.println(" %)");
}

// =================================
// 8. COMMANDE AUTOMATIQUE
// =================================

void controlDevices() {

  // POMPE (hysteresis)
  if (soilPercent < SOIL_START) {
    pumpState = true;
  } else if (soilPercent >= SOIL_STOP) {
    pumpState = false;
  }

  digitalWrite(RELAY_PUMP, pumpState ? RELAY_ON : RELAY_OFF);

  // LAMPE (automatique seulement si lampAuto = true)
  if (lampAuto) {
    if (lightValue < LIGHT_DARK) {
      lampState = true;
    } else if (lightValue > LIGHT_BRIGHT) {
      lampState = false;
    }
  }

  digitalWrite(RELAY_LAMP, lampState ? RELAY_ON : RELAY_OFF);

  Serial.print("Pompe : ");
  Serial.println(pumpState ? "ON" : "OFF");

  Serial.print("Lampe : ");
  Serial.print(lampState ? "ON" : "OFF");
  Serial.println(lampAuto ? " (auto)" : " (manuel)");
}

// =================================
// 9. ENVOI DES DONNEES MQTT
// =================================

void sendToThingsBoard() {

  if (!client.connected()) {
    return;
  }

  char payload[220];

  snprintf(
    payload,
    sizeof(payload),
    "{\"soil\":%d,"
    "\"light\":%d,"
    "\"lightPercent\":%d,"
    "\"pump\":%s,"
    "\"lamp\":%s,"
    "\"lampAuto\":%s}",
    soilPercent,
    lightValue,
    lightPercent,
    pumpState ? "true" : "false",
    lampState ? "true" : "false",
    lampAuto ? "true" : "false"
  );

  if (client.publish("v1/devices/me/telemetry", payload)) {
    Serial.println("\nDonnees envoyees a ThingsBoard :");
    Serial.println(payload);
  } else {
    Serial.println("Erreur d'envoi MQTT !");
  }
}

// =================================
// 10. SETUP
// =================================

void setup() {

  Serial.begin(115200);

  analogReadResolution(12);
  analogSetPinAttenuation(LDR_PIN, ADC_11db);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);

  pinMode(SOIL_PIN, INPUT);
  pinMode(LDR_PIN, INPUT);

  pinMode(RELAY_PUMP, OUTPUT);
  pinMode(RELAY_LAMP, OUTPUT);

  // Desactiver les relais au demarrage
  digitalWrite(RELAY_PUMP, RELAY_OFF);
  digitalWrite(RELAY_LAMP, RELAY_OFF);

  connectWiFi();

  client.setServer(TB_SERVER, TB_PORT);
  client.setCallback(mqttCallback);
  client.setSocketTimeout(10);
  client.setKeepAlive(30);

  Serial.println("\nJARDIN INTELLIGENT DEMARRE !");
}

// =================================
// 11. LOOP
// =================================

void loop() {

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!client.connected()) {
    tryReconnectMQTT();
  } else {
    client.loop();
  }

  unsigned long now = millis();

  // Lecture et commande toutes les 2 secondes (meme sans MQTT)
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readSensors();
    controlDevices();
  }

  // Envoi vers ThingsBoard toutes les 5 secondes
  if (now - lastSend >= SEND_INTERVAL) {
    lastSend = now;
    sendToThingsBoard();
  }
}