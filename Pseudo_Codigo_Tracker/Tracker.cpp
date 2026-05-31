#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <Adafruit_AHTX0.h>
#include <Wire.h>

// --- MAPEO DE PINES (Nivel 3) ---
#define PIN_MOSFET_SENSORS 12  // Activación de mosfet para energizar 
#define PIN_SOIL_MOISTURE  13  // ADC14 - Humedad del suelo 
#define PIN_LED            13  // Estado de transmisión 
#define I2C_SDA            33  // 
#define I2C_SCL            36  // 

// Configuración LoRa T-Beam 
#define SCK     5   // 
#define MISO    19  // 
#define MOSI    27  // 
#define SS      18  // 
#define RST     23  // 
#define DIO0    26  // 

// --- PARÁMETROS DE OPERACIÓN (Página 2) ---
const long FREQUENCY = 915E6;  // 915MHz 
const long BANDWIDTH = 250E3;  // BW= 250 kHz
const int SF = 10;             // SF10 
const int INTERVAL_MIN = 20;   // Intervalo= 20 min 

Adafruit_AHTX0 aht;

void setup() {
  Serial.begin(115200);
  
  // 1. INICIO: Despertar sensores 
  pinMode(PIN_MOSFET_SENSORS, OUTPUT);
  digitalWrite(PIN_MOSFET_SENSORS, HIGH); // Alimentar gate de MOSFETS 
  pinMode(PIN_LED, OUTPUT);
  
  // Inicializar I2C con pines del diagrama 
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(100); // Tiempo para estabilización de sensores

  // 2. SENSING: Adquirir datos 
  float temp = 0, hum = 0;
  int soil = 0;
  
  if (aht.begin()) {
    sensors_event_t sensor_hum, sensor_temp;
    aht.getEvent(&sensor_hum, &sensor_temp);
    temp = sensor_temp.temperature;
    hum = sensor_hum.relative_humidity;
  }
  soil = analogRead(PIN_SOIL_MOISTURE); // Lectura humedad suelo 

  // 3. APAGAR SENSORES: 
  digitalWrite(PIN_MOSFET_SENSORS, LOW);

  // 4. TRANSMISIÓN LORA: 
  setupLoRa();
  bool success = transmitData(temp, hum, soil);

  // 5. DEEP SLEEP: 
  Serial.println("Entrando en Deep Sleep...");
  esp_sleep_enable_timer_wakeup(INTERVAL_MIN * 60 * 1000000ULL);
  esp_deep_sleep_start();
}

void setupLoRa() {
  LoRa.setPins(SS, RST, DIO0);
  if (!LoRa.begin(FREQUENCY)) {
    while (1);
  }
  LoRa.setSignalBandwidth(BANDWIDTH);
  LoRa.setSpreadingFactor(SF);
}

bool transmitData(float t, float h, int s) {
  int intentos = 0; // Contador de intentos 
  
  while (intentos < 3) { // Más de 3 intentos? 
    Serial.print("Intento de envío: "); Serial.println(intentos + 1);
    
    // TX_DATA 
    LoRa.beginPacket();
    LoRa.print("T:"); LoRa.print(t);
    LoRa.print("|H:"); LoRa.print(h);
    LoRa.print("|S:"); LoRa.print(s);
    LoRa.endPacket();

    // WAIT_ACK: Esperar respuesta del Gateway 
    long startTime = millis();
    while (millis() - startTime < 5000) { // Ventana de 5 seg para ACK
      int packetSize = LoRa.parsePacket();
      if (packetSize) {
        digitalWrite(PIN_LED, HIGH); // Recibe respuesta, enciende LED 
        delay(500);
        digitalWrite(PIN_LED, LOW);
        return true; // Éxito
      }
    }
    
    // ERROR_RETRY 
    intentos++; // Contador suma 1 [cite: 132]
  }
  
  return false; // Falló tras 3 intentos [cite: 116]
}

void loop() {
  // Vacío por Deep Sleep
}
