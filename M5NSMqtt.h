#ifndef _M5NS_MQTT_H
#define _M5NS_MQTT_H

#include <Arduino.h>
#include "M5NSconfig.h"

void mqttInit();
void mqttLoop();
void mqttPublishState();
void mqttPublishDiscovery();
bool isMqttConnected();

#endif
