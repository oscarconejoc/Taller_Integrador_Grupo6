//Librerias Importantes
#include <Arduino.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <OneButton.h>
#include <TimeLib.h>
#include <TinyGPS++.h>
#include <WiFi.h>
#include <logger.h>
#include <ArduinoJson.h>
#include <APRS-Decoder.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <XPowersLib.h> // Cabecera corregida para la gestión de energía
#include <list>
#include <esp_task_wdt.h>

#define VERSION "23.36.01-Consolidated"

//Pines T-BEAM V1.2
#define OLED_SDA     21
#define OLED_SCL     22
#define OLED_RST     -1 // Administrado internamente o no requerido de forma física discreta
#define BUTTON_PIN   38
#define GPS_TX       12
#define GPS_RX       34
#define LORA_SCK     5
#define LORA_MISO    19
#define LORA_MOSI    27
#define LORA_CS      18
#define LORA_RST     23
#define LORA_IRQ     26

//Def variables
struct BeaconConfig {
  String callsign;
  String path;
  String message;
  int timeout;
  String symbol;
  String overlay;
  struct {
    bool active;
    int turn_min;
    int slow_rate;
    int slow_speed;
    int fast_rate;
    int fast_speed;
    int min_tx_dist;
    int min_bcn;
  } smart_beacon;
  bool enhance_precision;
};

//Configuracion del sistema
struct SystemConfiguration {
  bool debug;
  std::list<BeaconConfig> beacons;
  struct { bool tx; bool alt_message; } button;
  struct { long frequencyRx; long frequencyTx; int power; int spreadingFactor; long signalBandwidth; int codingRate4; } lora;
  struct { bool active; int io_pin; int start_delay; int end_delay; bool reverse; } ptt;
};

// Globales
logging::Logger logger;
SystemConfiguration Config;
Adafruit_SSD1306 display(128, 64, &Wire, OLED_RST);
XPowersAXP2101 pmu;
OneButton userButton(BUTTON_PIN, true, true);
HardwareSerial ss(1);
TinyGPSPlus gps;

// Gestion de perfiles (se cuenta con solo uno)
class InlineBeaconManager {
private:
  std::list<BeaconConfig> _beacons;
  std::list<BeaconConfig>::iterator _current;
public:
  void load(const std::list<BeaconConfig> &config) { _beacons = config; _current = _beacons.begin(); }
  std::list<BeaconConfig>::iterator getCurrent() const { return _current; }
  void next() { if (++_current == _beacons.end()) _current = _beacons.begin(); }
} BeaconMan;

// Display OLED
void setup_display() {
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3c, false, false)) {
    logger.log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, "SSD1306", "¡Fallo de asignación OLED!");
    while (true);
  }
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("LORA TRACKER V1.2");
  display.display();
}

void show_display(String header, String l1 = "", String l2 = "", String l3 = "", int wait = 0) {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println(header);
  display.setTextSize(1);
  if(l1 != "") { display.setCursor(0, 16); display.println(l1); }
  if(l2 != "") { display.setCursor(0, 26); display.println(l2); }
  if(l3 != "") { display.setCursor(0, 36); display.println(l3); }
  display.display();
  if (wait > 0) delay(wait);
}

// Control de Alimentacion (Lógica adaptada a la API pública de XPowersLib)
bool setup_pmu() {
  if (!pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, OLED_SDA, OLED_SCL)) return false;
  
  pmu.disableDC2();
  pmu.disableDC3();
  pmu.disableDC4();
  pmu.disableDC5();
  pmu.disableALDO1();
  pmu.disableALDO4();
  pmu.disableBLDO1();
  pmu.disableBLDO2();
  pmu.disableDLDO1();
  pmu.disableDLDO2();

  // AXP2101 no expone canal Vbackup en esta versión de la biblioteca.
  // Se establecen los voltajes de ALDO directamente para LoRa y GPS.
  pmu.setALDO2Voltage(3300); // LoRa VDD
  pmu.setALDO3Voltage(3300); // GPS VDD
  pmu.enableALDO2();

  pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_800MA);
  pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  return true;
}

void load_config() {
  Config.beacons.clear();
  Config.button.tx = false;
  Config.button.alt_message = false;
  Config.lora.frequencyTx = 433775000;
  Config.lora.power = 20;
  Config.lora.spreadingFactor = 12;
  Config.lora.signalBandwidth = 125000;
  Config.lora.codingRate4 = 5;
  Config.ptt.active = false;
  Config.ptt.io_pin = 0;
  Config.ptt.start_delay = 0;
  Config.ptt.end_delay = 0;
  Config.ptt.reverse = false;

  if (!SPIFFS.begin(true)) {
    logger.log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, "FS", "Error al montar SPIFFS.");
    return;
  }

  const char *paths[] = {
    "/tracker.json",
    "/Tracker.json",
    "/data.json",
    "/Data.json",
    "/data/tracker.json",
    "/data/Tracker.json"
  };

  File file;
  const char *openedPath = nullptr;
  for (const char *path : paths) {
    file = SPIFFS.open(path, FILE_READ);
    if (file && file.size() > 0) {
      openedPath = path;
      break;
    }
    if (file) {
      file.close();
    }
  }

  if (!file) {
    logger.log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, "Config", "No existe tracker.json ni data.json");
    show_display("ERROR", ".JSON", "Cargar valido", "", 2000);
  } else {
    DynamicJsonDocument data(4096);
    auto err = deserializeJson(data, file);
    size_t fileSize = file.size();
    file.close();
    if (err) {
      String errMsg = String(err.c_str());
      if (fileSize == 0) {
        errMsg = "Empty file";
      }
      logger.log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, "Config", "Error al parsear JSON (%s): %s", openedPath ? openedPath : "unknown", errMsg.c_str());
      show_display("ERROR", ".JSON invalido", errMsg, "", 3000);
    } else {
      if (openedPath) {
        show_display("JSON OK", openedPath, "", "", 1000);
      }
      Config.debug = data["debug"] | false;
      Config.debug = data["debug"] | false;
      JsonArray beacons = data["beacons"].as<JsonArray>();
      if (beacons.isNull()) {
        logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "Config", "No existe campo 'beacons' en JSON.");
        show_display("WARNING", "No beacons", "...", "", 2000);
      } else {
        for (JsonVariant v : beacons) {
          BeaconConfig bc;
          bc.callsign = v["callsign"].as<String>();
          bc.path = v["path"].as<String>();
          bc.message = v["message"].as<String>();
          bc.timeout = v["timeout"] | 1;
          bc.symbol = v["symbol"].as<String>();
          bc.overlay = v["overlay"].as<String>();
          bc.smart_beacon.active = v["smart_beacon"]["active"] | false;
          bc.smart_beacon.turn_min = v["smart_beacon"]["turn_min"] | 0;
          bc.smart_beacon.slow_rate = v["smart_beacon"]["slow_rate"] | 300;
          bc.smart_beacon.slow_speed = v["smart_beacon"]["slow_speed"] | 10;
          bc.smart_beacon.fast_rate = v["smart_beacon"]["fast_rate"] | 0;
          bc.smart_beacon.fast_speed = v["smart_beacon"]["fast_speed"] | 0;
          bc.smart_beacon.min_tx_dist = v["smart_beacon"]["min_tx_dist"] | 0;
          bc.smart_beacon.min_bcn = v["smart_beacon"]["min_bcn"] | 0;
          bc.enhance_precision = v["enhance_precision"] | false;
          Config.beacons.push_back(bc);
        }
      }
      Config.button.tx = data["button"]["tx"] | false;
      Config.button.alt_message = data["button"]["alt_message"] | false;
      Config.lora.frequencyTx = data["lora"]["frequency_tx"] | 433775000;
      Config.lora.power = data["lora"]["power"] | 20;
      Config.lora.spreadingFactor = data["lora"]["spreading_factor"] | 12;
      Config.lora.signalBandwidth = data["lora"]["signal_bandwidth"] | 125000;
      Config.lora.codingRate4 = data["lora"]["coding_rate4"] | 5;
      JsonObject pttData = data["ptt"].as<JsonObject>();
      if (pttData.isNull()) {
        pttData = data["ptt_output"].as<JsonObject>();
      }
      Config.ptt.active = pttData["active"] | false;
      Config.ptt.io_pin = pttData["io_pin"] | 0;
      Config.ptt.start_delay = pttData["start_delay"] | 0;
      Config.ptt.end_delay = pttData["end_delay"] | 0;
      Config.ptt.reverse = pttData["reverse"] | false;
    }
  }

  if (Config.beacons.empty()) {
    BeaconConfig defaultBeacon;
    defaultBeacon.callsign = "NOCALL";
    defaultBeacon.path = "WIDE1-1";
    defaultBeacon.message = "TRACKER";
    defaultBeacon.timeout = 60;
    defaultBeacon.symbol = "/";
    defaultBeacon.overlay = "!";
    defaultBeacon.smart_beacon.active = false;
    defaultBeacon.smart_beacon.turn_min = 0;
    defaultBeacon.smart_beacon.slow_rate = 300;
    defaultBeacon.smart_beacon.slow_speed = 10;
    defaultBeacon.smart_beacon.fast_rate = 0;
    defaultBeacon.smart_beacon.fast_speed = 0;
    defaultBeacon.smart_beacon.min_tx_dist = 0;
    defaultBeacon.smart_beacon.min_bcn = 0;
    defaultBeacon.enhance_precision = false;
    Config.beacons.push_back(defaultBeacon);
  }

  BeaconMan.load(Config.beacons);
}

//Formato APRS
char *s_min_nn(uint32_t min_nnnnn, int high_precision) {
  static char buf[6];
  min_nnnnn = min_nnnnn * 0.006;
  if (high_precision) { if ((min_nnnnn % 10) >= 5) min_nnnnn += 5; }
  else { if ((min_nnnnn % 1000) >= 500) min_nnnnn += 500; }
  
  if (high_precision < 2)
    sprintf(buf, "%02u.%02u", (unsigned int)((min_nnnnn / 100000) % 100), (unsigned int)((min_nnnnn / 1000) % 100));
  else
    sprintf(buf, "%c", (char)((min_nnnnn % 1000) / 11) + 33);
  return buf;
}

String padding(unsigned int number, unsigned int width) {
  String result = "";
  String num(number);
  for (unsigned int i = 0; i < (width - num.length()); i++) result.concat('0');
  result.concat(num);
  return result;
}

String create_lat_aprs_dao(RawDegrees lat) {
  char str[20];
  sprintf(str, "%02d%s%c", lat.deg, s_min_nn(lat.billionths, 1), lat.negative ? 'S' : 'N');
  return String(str);
}

String create_long_aprs_dao(RawDegrees lng) {
  char str[20];
  sprintf(str, "%03d%s%c", lng.deg, s_min_nn(lng.billionths, 1), lng.negative ? 'W' : 'E');
  return String(str);
}

String create_dao_aprs(RawDegrees lat, RawDegrees lng) {
  char str[10];
  sprintf(str, "!w%s", s_min_nn(lat.billionths, 2));
  sprintf(str + 3, "%s!", s_min_nn(lng.billionths, 2));
  return String(str);
}

//Boton 
static bool send_update = true;
void handle_tx_click() { send_update = true; }
void handle_next_beacon() {
  BeaconMan.next();
  show_display(BeaconMan.getCurrent()->callsign, BeaconMan.getCurrent()->message, "", "", 2000);
}

//Configuracion RF
bool lora_initialized = false;
void setup_lora() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_IRQ);
  
  logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "LoRa", "Inicializando LoRa...");
  delay(100);
  
  if (!LoRa.begin(Config.lora.frequencyTx)) {
    logger.log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, "LoRa", "¡Fallo al inicializar LoRa!");
    show_display("ERR LORA", "...", "Reintentando...", "", 3000);
    lora_initialized = false;
    return;
  }
  
  LoRa.setSpreadingFactor(Config.lora.spreadingFactor);
  LoRa.setSignalBandwidth(Config.lora.signalBandwidth);
  LoRa.setCodingRate4(Config.lora.codingRate4);
  LoRa.enableCrc();
  LoRa.setTxPower(Config.lora.power);
  
  logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "LoRa", "LoRa inicializado exitosamente");
  lora_initialized = true;
}

//FSM
enum Estado { GPS_CALIBRATION, GPS_ON, SENSING, BUILD_PACKET, TX_DATA, SLEEP, ERROR_RETRY };
static Estado estadoActual = GPS_CALIBRATION;
static Estado estadoFallo = GPS_ON;
static int contadorReintentos = 0;
static uint32_t timerProximoCiclo = 0;
static uint32_t timerInicio = 0;
static uint32_t gpsOnStart = 0;

// Variables FSM
static int calLecturas = 0;
static uint32_t calTimerEspera = 0;
static bool calEsperandoFix = true;
static bool calEsperandoPausa = false;
static uint32_t calTiempoTotal = 0;
static const uint32_t calIntervalos[] = { 180000, 120000, 60000 };

#define UMBRAL_VELOCIDAD      5
#define TIMEOUT_GPS           60000
#define MAX_REINTENTOS        3
#define INTERVALO_MOVIMIENTO  300000   
#define INTERVALO_REPOSO      3600000  

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  // Deshabilitar watchdog durante inicialización
  esp_task_wdt_deinit();
  
  logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "Main", "=== INICIANDO SISTEMA ===");
  
  Wire.begin(OLED_SDA, OLED_SCL);
  delay(100);
  
  // Inicializar PMU
  if (setup_pmu()) {
    logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "PMU", "¡Inicialización exitosa!");
  } else {
    logger.log(logging::LoggerLevel::LOGGER_LEVEL_WARN, "PMU", "PMU no disponible, continuando...");
  }
  
  delay(200);
  pmu.enableDC1(); // Alimentación pantalla
  delay(100);
  
  // Display
  setup_display();
  show_display("APRS", "Control vial", "Version: " VERSION, "", 2000);
  
  // Cargar configuración
  logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "Main", "Cargando configuración...");
  delay(100);
  load_config();
  delay(500);
  
  logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "Main", "Configuración cargada");
  if (!BeaconMan.getCurrent()->callsign.isEmpty()) {
    logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "Main", "Callsign: %s", BeaconMan.getCurrent()->callsign.c_str());
  }
  
  // GPS Serial
  logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "GPS", "Inicializando GPS...");
  delay(100);
  ss.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX); // rxPin, txPin
  logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "GPS", "Serial GPS iniciado");
  delay(500);
  
  // LoRa (con reintentos)
  logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "Main", "Inicializando LoRa...");
  delay(100);
  for (int i = 0; i < 3; i++) {
    setup_lora();
    if (lora_initialized) break;
    logger.log(logging::LoggerLevel::LOGGER_LEVEL_WARN, "Main", "Reintentando LoRa (%d/3)", i+1);
    delay(500);
  }
  
  delay(500);
  
  // WiFi y BT apagados
  WiFi.mode(WIFI_OFF);
  btStop();
  
  // Botón
  if (Config.button.tx) userButton.attachClick(handle_tx_click);
  if (Config.button.alt_message) userButton.attachLongPressStart(handle_next_beacon);
  
  // GPS ON
  pmu.enableALDO3();
  delay(100);
  
  logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "Main", "Sistema listo - Iniciando FSM");
  
  // Reactivar watchdog
  esp_task_wdt_init(30, true); // 30 segundos
  
  timerProximoCiclo = INTERVALO_REPOSO;
}

//Configuracion GPS
void loop() {
  // Alimentar al watchdog
  esp_task_wdt_reset();
  
  while (ss.available() > 0) gps.encode(ss.read());
  userButton.tick();

  switch (estadoActual) {
    case GPS_CALIBRATION: {
      static bool calIniciado = false;
      if (!calIniciado) {
        calIniciado = true; 
        calTiempoTotal = millis(); 
        calTimerEspera = millis(); 
        calLecturas = 0;
        show_display("GPS", "Reading...", "Lecturas: 0/4", "");
      }
      
      // Timeout: si no tienes fix en 15 minutos, salta a SLEEP
      if (millis() - calTiempoTotal > 900000) {
        logger.log(logging::LoggerLevel::LOGGER_LEVEL_WARN, "GPS", "Timeout...");
        estadoActual = SLEEP; 
        calIniciado = false; 
        return;
      }
      
      if (calEsperandoFix) {
        if (gps.location.isValid() && gps.location.isUpdated()) {
          calLecturas++;
          show_display("GPS", String("Lectura ") + String(calLecturas) + "/4", String(gps.location.lat(),4), "", 2000);
          if (calLecturas >= 4) {
            logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "GPS", "Calibración completada");
            estadoActual = SLEEP; 
            calIniciado = false; 
            return;
          }
          calEsperandoFix = false; 
          calEsperandoPausa = true; 
          calTimerEspera = millis();
        }
        return;
      }
      
      if (calEsperandoPausa) {
        uint32_t intervalo = calIntervalos[calLecturas - 1];
        if (millis() - calTimerEspera >= intervalo) {
          calEsperandoPausa = false; 
          calEsperandoFix = true; 
          calTimerEspera = millis();
        }
      }
      break;
    }

    case GPS_ON:
      pmu.enableALDO3(); // Encender GPS (Corregido)
      if (gpsOnStart == 0) {
        gpsOnStart = millis();
        show_display("GPS", "Fix...", "", "", 1000);
      }
      if (gps.location.isValid() && gps.location.isUpdated()) {
        gpsOnStart = 0;
        estadoActual = SENSING;
      } else if (millis() - gpsOnStart > TIMEOUT_GPS) {
        gpsOnStart = 0;
        estadoFallo = GPS_ON; estadoActual = ERROR_RETRY;
      }
      break;

    case SENSING:
      if (!gps.location.isValid()) { estadoFallo = SENSING; estadoActual = ERROR_RETRY; return; }
      estadoActual = BUILD_PACKET;
      break;

    case BUILD_PACKET: {
      APRSMessage msg;
      msg.setSource(BeaconMan.getCurrent()->callsign);
      msg.setPath(BeaconMan.getCurrent()->path);
      msg.setDestination("APLT00");

      String lat = create_lat_aprs_dao(gps.location.rawLat());
      String lng = create_long_aprs_dao(gps.location.rawLng());
      String dao = create_dao_aprs(gps.location.rawLat(), gps.location.rawLng());
      
      int alt_int = max(-99999, min(999999, (int)gps.altitude.feet()));
      String alt = (alt_int < 0) ? String("/A=-") + padding(alt_int * -1, 5) : String("/A=") + padding(alt_int, 6);
      String curso_vel = padding(max(0, min(360, (int)gps.course.deg())), 3) + "/" + padding(max(0, min(999, (int)gps.speed.knots())), 3);

      String cuerpo = String("!") + lat + BeaconMan.getCurrent()->overlay + lng + BeaconMan.getCurrent()->symbol + curso_vel + alt + BeaconMan.getCurrent()->message + " " + dao;
      msg.getBody()->setData(cuerpo);
      
      timerProximoCiclo = (gps.speed.kmph() > UMBRAL_VELOCIDAD) ? INTERVALO_MOVIMIENTO : INTERVALO_REPOSO;
      estadoActual = TX_DATA;
      break;
    }

    case TX_DATA: {
      pmu.disableALDO3(); // Apagar GPS para ahorrar energía (Corregido)
      
      // Verificar que LoRa esté inicializado
      if (!lora_initialized) {
        logger.log(logging::LoggerLevel::LOGGER_LEVEL_WARN, "TX", "LoRa no inicializado, reintentando...");
        show_display("LORA", "Redo...", "", "", 1000);
        setup_lora();
        if (!lora_initialized) {
          estadoFallo = TX_DATA; 
          estadoActual = ERROR_RETRY;
          break;
        }
      }
      
      // Re-encapsulado rápido para transmisión física
      APRSMessage msg;
      msg.setSource(BeaconMan.getCurrent()->callsign);
      msg.setPath(BeaconMan.getCurrent()->path);
      msg.setDestination("APLT00");
      String lat = create_lat_aprs_dao(gps.location.rawLat());
      String lng = create_long_aprs_dao(gps.location.rawLng());
      String dao = create_dao_aprs(gps.location.rawLat(), gps.location.rawLng());
      String cuerpo = String("!") + lat + BeaconMan.getCurrent()->overlay + lng + BeaconMan.getCurrent()->symbol + String("000/000") + String("/A=000000") + BeaconMan.getCurrent()->message + " " + dao;
      msg.getBody()->setData(cuerpo);
      String trama = msg.encode();

      show_display("<< TX >>", trama.substring(0, 21), "", "", 0);
      logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "TX", "Enviando trama: %s", trama.substring(0, 30).c_str());

      LoRa.beginPacket();
      LoRa.write('<'); LoRa.write(0xFF); LoRa.write(0x01);
      LoRa.write((const uint8_t *)trama.c_str(), trama.length());
      
      if (LoRa.endPacket()) {
        logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "TX", "¡Transmisión exitosa!");
        show_display("TX", "Exitoso", "", "", 2000);
        estadoActual = SLEEP;
      } else {
        logger.log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, "TX", "Fallo en transmisión");
        estadoFallo = TX_DATA; 
        estadoActual = ERROR_RETRY;
      }
      break;
    }

    case SLEEP:
      static bool sleepIniciado = false;
      if (!sleepIniciado) { timerInicio = millis(); sleepIniciado = true; contadorReintentos = 0; }
      show_display("SLEEP", "Proximo envio en:", String((timerProximoCiclo - (millis() - timerInicio)) / 1000) + "s", "");
      
      if (millis() - timerInicio >= timerProximoCiclo || send_update) {
        send_update = false; sleepIniciado = false; estadoActual = GPS_ON;
      }
      break;

    case ERROR_RETRY:
      contadorReintentos++;
      if (contadorReintentos < MAX_REINTENTOS) {
        estadoActual = estadoFallo;
      } else {
        timerProximoCiclo = INTERVALO_REPOSO; contadorReintentos = 0; estadoActual = SLEEP;
      }
      break;
  }
}
