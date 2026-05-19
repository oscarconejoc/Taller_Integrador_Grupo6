#include <Arduino.h>
#include <LoRa.h>
#include <TinyGPS++.h>
#include <axp202x.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>

// Definición de pines
#define SCK     5
#define MISO    19
#define MOSI    27
#define SS      18
#define RST     23
#define DIO0    26

TinyGPSPlus gps;
HardwareSerial GPS_Serial(1);
AXP202X_Class axp;

// Variables de configuración 
String callsign;
float frequency;

void loadConfig() {
    if (!SPIFFS.begin(true)) return;
    File configFile = SPIFFS.open("/data.json", "r");
    if (!configFile) return;

    StaticJsonDocument<200> doc;
    deserializeJson(doc, configFile);
    callsign = doc["callsign"] | "NOCALL";
    frequency = doc["freq"] | 433.775;
    configFile.close();
}

void setup() {
    Serial.begin(115200);
    GPS_Serial.begin(9600, SERIAL_8N1, 34, 12); // Pines GPS estándar T-Beam

    // Inicializar gestión de energía 
    // Para encender el GPS y el chip LoRa
    Wire.begin(21, 22);
    if (!axp.begin(Wire, AXP192_SLAVE_ADDRESS)) {
        axp.setPowerOutPut(AXP192_LDO2, AXP202_ON); // GPS
        axp.setPowerOutPut(AXP192_LDO3, AXP202_ON); // LoRa
        axp.setPowerOutPut(AXP192_DCDC1, AXP202_ON); // OLED/Main
    }

    loadConfig();

    // Configurar LoRa
    SPI.begin(SCK, MISO, MOSI, SS);
    LoRa.setPins(SS, RST, DIO0);
    if (!LoRa.begin(frequency * 1E6)) {
        Serial.println("Error al iniciar LoRa");
        while (1);
    }
    
    // Configuración típica para APRS LoRa
    LoRa.setSpreadingFactor(12);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setCodingRate4(5);
    LoRa.enableCrc();
}

// Función para convertir lat/lon al formato DDMM.mm
String formatLocation(double loc, bool isLat) {
    char buf[12];
    char hemisphere = isLat ? (loc >= 0 ? 'N' : 'S') : (loc >= 0 ? 'E' : 'W');
    loc = abs(loc);
    int deg = (int)loc;
    double min = (loc - deg) * 60.0;
    if (isLat) sprintf(buf, "%02d%05.2f%c", deg, min, hemisphere);
    else sprintf(buf, "%03d%05.2f%c", deg, min, hemisphere);
    return String(buf);
}

void sendAPRS() {
    if (gps.location.isValid()) {
        // Construcción del paquete APRS
        // Formato: <Callsign> + > + APREXP (TOCALL) + : + ! + Lat + / + Lon + [ (Símbolo)
        String packet = callsign + ">APLG01,WIDE1-1:!" + 
                        formatLocation(gps.location.lat(), true) + "/" + 
                        formatLocation(gps.location.lng(), false) + "[" + 
                        " T-Beam Tracker";

        LoRa.beginPacket();
        LoRa.write('<'); // Cabecera necesaria para algunos IGates
        LoRa.print(packet);
        LoRa.endPacket();
        
        Serial.println("Paquete enviado: " + packet);
    }
}

void loop() {
    // Leer datos del GPS
    while (GPS_Serial.available() > 0) {
        gps.encode(GPS_Serial.read());
    }

    // Enviar cada 1 minuto si hay GPS
    static unsigned long lastSend = 0;
    if (millis() - lastSend > 60000) {
        sendAPRS();
        lastSend = millis();
    }
}