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
#include <XPowersAXP2101.tpp>
#include <list>

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

// Control de Alimentacion
bool setup_pmu() {
  if (!pmu.begin(Wire)) return false;
  pmu.disablePowerOutput(XPOWERS_DCDC2);
  pmu.disablePowerOutput(XPOWERS_DCDC3);
  pmu.disablePowerOutput(XPOWERS_DCDC4);
  pmu.disablePowerOutput(XPOWERS_DCDC5);
  pmu.disablePowerOutput(XPOWERS_ALDO1);
  pmu.disablePowerOutput(XPOWERS_ALDO4);
  pmu.disablePowerOutput(XPOWERS_BLDO1);
  pmu.disablePowerOutput(XPOWERS_BLDO2);
  pmu.disablePowerOutput(XPOWERS_DLDO1);
  pmu.disablePowerOutput(XPOWERS_DLDO2);

  pmu.setPowerChannelVoltage(XPOWERS_VBACKUP, 3300);
  pmu.enablePowerOutput(XPOWERS_VBACKUP);
  pmu.setPowerChannelVoltage(XPOWERS_ALDO2, 3300); // LoRa VDD
  pmu.setPowerChannelVoltage(XPOWERS_ALDO3, 3300); // GPS VDD
  pmu.enablePowerOutput(XPOWERS_ALDO2);
  pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_800MA);
  pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  return true;
}

//Lectura json
void load_config() {
  if (!SPIFFS.begin(true)) {
    logger.log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, "FS", "Error al montar SPIFFS.");
    return;
  }
  File file = SPIFFS.open("/tracker.json");
  if (!file) {
    logger.log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, "Config", "No existe tracker.json");
    return;
  }
  DynamicJsonDocument data(2048);
  deserializeJson(data, file);
  file.close();

  Config.debug = data["debug"] | false;
  JsonArray beacons = data["beacons"].as<JsonArray>();
  for (JsonVariant v : beacons) {
    BeaconConfig bc;
    bc.callsign = v["callsign"].as<String>();
    bc.path = v["path"].as<String>();
    bc.message = v["message"].as<String>();
    bc.timeout = v["timeout"] | 1;
    bc.symbol = v["symbol"].as<String>();
    bc.overlay = v["overlay"].as<String>();
    bc.smart_beacon.active = v["smart_beacon"]["active"] | false;
    bc.smart_beacon.slow_rate = v["smart_beacon"]["slow_rate"] | 300;
    bc.smart_beacon.slow_speed = v["smart_beacon"]["slow_speed"] | 10;
    Config.beacons.push_back(bc);
  }
  Config.button.tx = data["button"]["tx"] | false;
  Config.button.alt_message = data["button"]["alt_message"] | false;
  Config.lora.frequencyTx = data["lora"]["frequency_tx"] | 433775000;
  Config.lora.power = data["lora"]["power"] | 20;
  Config.lora.spreadingFactor = data["lora"]["spreading_factor"] | 12;
  Config.lora.signalBandwidth = data["lora"]["signal_bandwidth"] | 125000;
  Config.lora.codingRate4 = data["lora"]["coding_rate4"] | 5;
  
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
void setup_lora() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_IRQ);
  if (!LoRa.begin(Config.lora.frequencyTx)) {
    show_display("ERROR", "Fallo LoRa", "", "", 0);
    while (true);
  }
  LoRa.setSpreadingFactor(Config.lora.spreadingFactor);
  LoRa.setSignalBandwidth(Config.lora.signalBandwidth);
  LoRa.setCodingRate4(Config.lora.codingRate4);
  LoRa.enableCrc();
  LoRa.setTxPower(Config.lora.power);
}

//FSM
enum Estado { GPS_CALIBRATION, GPS_ON, SENSING, BUILD_PACKET, TX_DATA, SLEEP, ERROR_RETRY };
static Estado estadoActual = GPS_CALIBRATION;
static Estado estadoFallo = GPS_ON;
static int contadorReintentos = 0;
static uint32_t timerProximoCiclo = 0;
static uint32_t timerInicio = 0;

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
  Wire.begin(OLED_SDA, OLED_SCL);
  
  if (setup_pmu()) logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "PMU", "¡Inicialización exitosa!");
  pmu.enablePowerOutput(XPOWERS_DCDC1); // Alimentación pantalla
  
  setup_display();
  show_display("TEC APRS", "Rastreador Unificado", "Version: " VERSION, "", 2000);
  
  load_config();
  ss.begin(9600, SERIAL_8N1, GPS_TX, GPS_RX);
  setup_lora();

  WiFi.mode(WIFI_OFF);
  btStop();

  if (Config.button.tx) userButton.attachClick(handle_tx_click);
  if (Config.button.alt_message) userButton.attachLongPressStart(handle_next_beacon);

  pmu.enablePowerOutput(XPOWERS_ALDO3); // Encender GPS
  timerProximoCiclo = INTERVALO_REPOSO;
}
//Configuracion GPS
void loop() {
  while (ss.available() > 0) gps.encode(ss.read());
  userButton.tick();

  switch (estadoActual) {
    case GPS_CALIBRATION: {
      static bool calIniciado = false;
      if (!calIniciado) {
        calIniciado = true; calTiempoTotal = millis(); calTimerEspera = millis(); calLecturas = 0;
        show_display("CAL GPS", "Validando...", "Lecturas: 0/4", "");
      }
      if (millis() - calTiempoTotal > 900000) { // Timeout 15 min
        estadoActual = SLEEP; calIniciado = false; return;
      }
      if (calEsperandoFix) {
        if (gps.location.isValid() && gps.location.isUpdated()) {
          calLecturas++;
          show_display("CAL OK", "Lectura " + String(calLecturas) + "/4", String(gps.location.lat(),4), "", 2000);
          if (calLecturas >= 4) {
            estadoActual = SLEEP; calIniciado = false; return;
          }
          calEsperandoFix = false; calEsperandoPausa = true; calTimerEspera = millis();
        }
        return;
      }
      if (calEsperandoPausa) {
        uint32_t intervalo = calIntervalos[calLecturas - 1];
        if (millis() - calTimerEspera >= intervalo) {
          calEsperandoPausa = false; calEsperandoFix = true; calTimerEspera = millis();
        }
      }
      break;
    }

    case GPS_ON:
      pmu.enablePowerOutput(XPOWERS_ALDO3);
      timerInicio = millis();
      if (gps.location.isValid() && gps.location.isUpdated()) {
        estadoActual = SENSING;
      } else if (millis() - timerInicio > TIMEOUT_GPS) {
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
      String alt = (alt_int < 0) ? "/A=-" + padding(alt_int * -1, 5) : "/A=" + padding(alt_int, 6);
      String curso_vel = padding(max(0, min(360, (int)gps.course.deg())), 3) + "/" + padding(max(0, min(999, (int)gps.speed.knots())), 3);

      String cuerpo = "!" + lat + BeaconMan.getCurrent()->overlay + lng + BeaconMan.getCurrent()->symbol + curso_vel + alt + BeaconMan.getCurrent()->message + " " + dao;
      msg.getBody()->setData(cuerpo);
      
      timerProximoCiclo = (gps.speed.kmph() > UMBRAL_VELOCIDAD) ? INTERVALO_MOVIMIENTO : INTERVALO_REPOSO;
      estadoActual = TX_DATA;
      break;
    }

    case TX_DATA: {
      pmu.disablePowerOutput(XPOWERS_ALDO3); // Apagar GPS para ahorrar energía
      
      // Re-encapsulado rápido para transmisión física
      APRSMessage msg;
      msg.setSource(BeaconMan.getCurrent()->callsign);
      msg.setPath(BeaconMan.getCurrent()->path);
      msg.setDestination("APLT00");
      String lat = create_lat_aprs_dao(gps.location.rawLat());
      String lng = create_long_aprs_dao(gps.location.rawLng());
      String dao = create_dao_aprs(gps.location.rawLat(), gps.location.rawLng());
      String cuerpo = "!" + lat + BeaconMan.getCurrent()->overlay + lng + BeaconMan.getCurrent()->symbol + "000/000" + "/A=000000" + BeaconMan.getCurrent()->message + " " + dao;
      msg.getBody()->setData(cuerpo);
      String trama = msg.encode();

      show_display("<< TX >>", trama.substring(0, 21), "", "", 0);

      LoRa.beginPacket();
      LoRa.write('<'); LoRa.write(0xFF); LoRa.write(0x01);
      LoRa.write((const uint8_t *)trama.c_str(), trama.length());
      
      if (LoRa.endPacket()) {
        show_display("TX OK", "Enviado con éxito", "", "", 2000);
        estadoActual = SLEEP;
      } else {
        estadoFallo = TX_DATA; estadoActual = ERROR_RETRY;
      }
      break;
    }

    case SLEEP:
      static bool sleepIniciado = false;
      if (!sleepIniciado) { timerInicio = millis(); sleepIniciado = true; contadorReintentos = 0; }
      show_display("SLEEP", "Próximo envío en:", String((timerProximoCiclo - (millis() - timerInicio)) / 1000) + "s", "");
      
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
