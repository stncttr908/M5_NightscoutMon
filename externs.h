#ifndef _EXTERNS_H_
#define _EXTERNS_H_

#include <Arduino.h>
#include <WebServer.h>
#include "M5NSconfig.h"
#include <WiFiMulti.h>
#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>

extern WebServer w3srv;
extern tConfig cfg;
extern struct NSinfo ns;
extern WiFiMulti WiFiMultiple;
extern bool mDNSactive;
extern int8_t getBatteryLevel();
extern void draw_page();
extern String M5NSversion;
extern int dispPage;
extern int maxPage;
extern void setPageIconPos(int page);
extern uint8_t lcdBrightness;
extern DynamicJsonDocument JSONdoc;
extern void addErrorLog(int code);
extern void resetConsecutiveSyncFailures();
extern void clearErrorLog();
extern void drawLogWarningIcon();
extern uint32_t consecutiveSyncFailures;
extern void drawIcon(int16_t x, int16_t y, const uint8_t *bitmap, uint16_t color);
extern uint16_t calcCRC(const char *str);

// REST API and runtime state helpers
extern uint32_t refreshIntervalSec;
extern bool screenOn;
extern uint8_t savedBrightness;
extern time_t snoozeUntil;
extern int snoozeMult;
extern void triggerSnooze();
extern void setSnooze(int minutes);
extern int getSnoozeRemainingMinutes();
extern int getSnoozeRemainingSeconds();
extern void cycleBrightness();
extern void setBrightness(uint8_t val);
extern void cyclePage();
extern void setPage(int page);
extern void setScreenPower(bool on);
extern void toggleScreenPower();
extern void resetNextRead();
extern void checkNightMode();
extern bool isNightModeActive();

extern void getPowerTelemetry(String &powerSource, bool &isCharging, int &batPercentage, float &batVoltage);

// MQTT helpers
extern void mqttInit();
extern void mqttLoop();
extern void mqttPublishState();
extern void mqttPublishDiscovery();
extern bool isMqttConnected();

#endif
