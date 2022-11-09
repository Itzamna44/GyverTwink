/*
  Скетч к проекту "GyverTwink"
  - Страница проекта (схемы, описания): https://alexgyver.ru/gyvertwink/
  - Исходники на GitHub: https://github.com/AlexGyver/GyverTwink
  Проблемы с загрузкой? Читай гайд для новичков: https://alexgyver.ru/arduino-first/
  Нормально работает только на последних версиях ядра esp8266
  AlexGyver, AlexGyver Technologies, 2021
  Alexey, MicrofCorp, 2021
*/

/*
  1.1 - исправлена калибровка больше 255 светодиодов
  1.2 - исправлена ошибка с калибровкой
  1.2 - WEBServer (by MicrofCorp) - добавленно HTTP управление
*/

/*
  Мигает синим - открыт портал
  Мигает жёлтым - подключаемся к точке
  Мигнул зелёным - подключился к точке
  Мигнул красным - ошибка подключения к точке
  Мигнул розовым - создал точку
*/

// ================ НАСТРОЙКИ ================
#define BTN_PIN D3      // пин кнопки
#define BTN_TOUCH 0     // 1 - сенсорная кнопка, 0 - нет

//#define LED_PIN D1      // пин ленты
#define LED_TYPE WS2812 // чип ленты
#define LED_ORDER RGB   // порядок цветов ленты

#define NUM_STRIPS 3
#define NUM_LEDS_PER_STRIP 100
#define NUM_LEDS (NUM_LEDS_PER_STRIP * NUM_STRIPS)

// имя точки в режиме AP
#define GT_AP_SSID "GyverTwink"
#define GT_AP_PASS "12345678"
#define DEBUG_SERIAL_GT   // раскомментируй, чтобы включить отладку
#define Arduino_OTA // раскомментируй, чтобы включить обновление по воздуху

// ================== LIBS ==================
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <SimplePortal.h>
//#include "lib\SimplePortal\src\SimplePortal.h"
#include <FastLED.h>
#include <EEManager.h>
#include <EncButton.h>
#include "palettes.h"
#include "Timer.h"
#ifdef Arduino_OTA
  #include <ArduinoOTA.h> //библиотека для ArduОТА
#endif

// ================== OBJECTS ==================
WiFiServer server(80);
ESP8266WebServer webserver(80);
WiFiUDP udp;
EEManager EEwifi(portalCfg);
CRGB leds[NUM_LEDS];
EncButton<EB_TICK, BTN_PIN> btn;
IPAddress myIP;

// ================== EEPROM BLOCKS ==================
struct Cfg {
  uint16_t strAm = 3;
  uint16_t ledAm = 100;
  bool power = 0;
  byte bright = 100;
  bool autoCh = 0;
  bool rndCh = 0;
  byte prdCh = 1;
  bool turnOff = 0;
  byte offTmr = 60;
};
Cfg cfg;
EEManager EEcfg(cfg);

byte xy[NUM_LEDS][2];
EEManager EExy(xy);

struct MM {
  byte minY = 0;
  byte maxY = 255;
  byte minX = 0;
  byte maxX = 255;
  byte w = 255;
  byte h = 255;
};
MM mm;
EEManager EEmm(mm);

#define ACTIVE_PALETTES 11
struct Effects {
  bool fav = true;
  byte scale = 50;
  byte speed = 150;
};
Effects effs[ACTIVE_PALETTES * 2];
EEManager EEeff(effs);

// ================== MISC DATA ==================
Timer forceTmr(30000, false);
Timer switchTmr(0, false);
Timer offTmr(60000, false);
bool calibF = false;
byte curEff = 0;
byte forceEff = 0;

#ifdef DEBUG_SERIAL_GT
#define DEBUGLN(x) Serial.println(x)
#define DEBUG(x) Serial.print(x)
#else
#define DEBUGLN(x)
#define DEBUG(x)
#endif

// ================== SETUP ==================
void setup() {
#ifdef DEBUG_SERIAL_GT
  Serial.begin(115200);
  DEBUGLN();
#endif
  delay(200);
  if (BTN_TOUCH) btn.setButtonLevel(HIGH);
  startStrip();
  EEPROM.begin(2048); // с запасом!

  // если это первый запуск или щелчок по кнопке, открываем портал
  if (EEwifi.begin(0, 'a') || checkButton()) portalRoutine();

  // создаём точку или подключаемся к AP
  if (portalCfg.mode == WIFI_AP || (portalCfg.mode == WIFI_STA && portalCfg.SSID[0] == '\0')) setupAP();
  else setupSTA();
  DEBUGLN(myIP);

  EEcfg.begin(EEwifi.nextAddr(), 'a');
  EEeff.begin(EEcfg.nextAddr(), 'a');
  EEmm.begin(EEeff.nextAddr(), (uint8_t)NUM_LEDS);
  EExy.begin(EEmm.nextAddr(), (uint8_t)NUM_LEDS);

  switchTmr.setPrd(cfg.prdCh * 60000ul);
  if (cfg.autoCh) switchTmr.restart();
  switchEff();
  cfg.turnOff = false;
  //FastLED.setLeds(leds, cfg.ledAm);
  udp.begin(8888);
  
  #ifdef Arduino_OTA
    ArduinoOTA.begin();
  #endif

  htmlstart();
}

// ================== LOOP ==================
void loop() {

  #ifdef Arduino_OTA
    ArduinoOTA.handle();
  #endif

  button();   // опрос кнопки
  
  webserver.handleClient(); //обновление web сервера

  // менеджер епром
  EEcfg.tick();
  EEeff.tick();

  parsing();  // парсим udp

  // таймер принудительного показа эффектов
  if (forceTmr.ready()) {
    forceTmr.stop();
    switchEff();
  }

  // форс выключен и настало время менять эффект
  if (!forceTmr.state() && switchTmr.ready()) switchEff();

  // таймер выключения
  if (offTmr.ready()) {
    offTmr.stop();
    cfg.turnOff = false;
    cfg.power = false;
    FastLED.setBrightness(0);
    FastLED.show();
    EEcfg.update();
    DEBUGLN("Off tmr");
  }

  // показываем эффект, если включены
  if (!calibF && cfg.power) effects();
}
